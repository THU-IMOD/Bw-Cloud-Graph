#include "bw_graph/service/graph_service.h"
#include "bw_graph/storage/s3_graph.h"

#include "bw_graph/db/multiscale_store.h"
#include "bw_graph/part/louvain.h"

#include <algorithm>
#include <array>
#include <bit>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/container/F14Map.h>
#include <folly/hash/Hash.h>
#include <mutex>
#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>
#include <set>
#include <shared_mutex>
#include <unordered_set>

namespace bw_graph::service {
namespace {
void checked(const rocksdb::Status& status) {
  if (!status.ok())
    throw std::runtime_error(status.ToString());
}
std::string key(char tag, uint64_t id) {
  std::string result(9, '\0');
  result[0] = tag;
  for (int i = 8; i; --i) {
    result[i] = static_cast<char>(id);
    id >>= 8;
  }
  return result;
}
uint64_t decode(const rocksdb::Slice& bytes) {
  if (bytes.size() != 9)
    throw std::runtime_error("Invalid ID mapping key");
  uint64_t id = 0;
  for (size_t i = 1; i < 9; ++i)
    id = (id << 8) | uint8_t(bytes[i]);
  return id;
}
// Operation stripes below protect compound check/update sequences. The table
// itself also permits different stripes to insert/erase while readers run.
template <class Key, class Value>
struct concurrent_table_t : folly::ConcurrentHashMap<Key, Value> {
  bool contains(const Key& k) const { return this->find(k) != this->end(); }
};
struct id_mapper_t {
  concurrent_table_t<std::string, uint64_t> forward;
  concurrent_table_t<uint64_t, std::string> reverse;
  void insert(const std::string& id, uint64_t inner) {
    if (forward.contains(id) || reverse.contains(inner))
      throw std::invalid_argument("Duplicate external ID or parallel edge");
    reverse.emplace(inner, id);
    try {
      forward.emplace(id, inner);
    } catch (...) {
      reverse.erase(inner);
      throw;
    }
  }
  void erase(uint64_t inner) {
    forward.erase(reverse.at(inner));
    reverse.erase(inner);
  }
};
multiscale_buf_options_t budgets() {
  multiscale_buf_options_t options;
  options.groups = {{{1, 12288}, {1, 256}, {1, 16}, {1, 2}, {0, 0}, {1, 64}}};
  return options; // 64 MiB CSR + 256 KiB Delta; decoded metadata is additional.
}
struct graph_t {
  // Shared for point operations; exclusive for allocation, deletion, snapshots
  // and close. Acquire gate before stripes, then stripes in ascending order.
  std::shared_mutex mutex;
  std::array<std::shared_mutex, 512> stripes;
  static size_t entity_stripe(bool edge, uint64_t id) {
    return folly::hash::hash_combine(edge ? 1 : 0, id) % 512;
  }
  static size_t external_stripe(bool edge, const std::string& id) {
    return folly::hash::hash_combine(edge ? 3 : 2, id) % 512;
  }
  auto write_stripes(std::initializer_list<size_t> indexes) {
    if (indexes.size() > 4)
      throw std::logic_error("Too many operation stripes");
    std::array<size_t, 4> sorted{};
    std::copy(indexes.begin(), indexes.end(), sorted.begin());
    auto end = sorted.begin() + indexes.size();
    std::sort(sorted.begin(), end);
    end = std::unique(sorted.begin(), end);
    std::array<std::unique_lock<std::shared_mutex>, 4> locks;
    for (size_t i = 0; i < size_t(end - sorted.begin()); ++i)
      locks[i] = std::unique_lock(stripes[sorted[i]]);
    return locks;
  }
  std::unique_ptr<s3_graph_t> cloud;
  std::unique_ptr<multiscale_store_t> store;
  std::unique_ptr<rocksdb::DB> metadata;
  id_mapper_t vertices, edges;
  size_t leases{0};
  bool created{false};
  std::atomic<bool> closed{false};
  std::atomic<bool> poisoned{false};
  explicit graph_t(const std::filesystem::path& path) {
    const bool local_fresh = std::filesystem::create_directory(path);
    if (s3_enabled()) cloud = std::make_unique<s3_graph_t>(path);
    const bool fresh = cloud ? !cloud->exists() : local_fresh;
    created = fresh;
    if (cloud && fresh && !local_fresh)
      throw std::runtime_error("S3 graph absent but local directory exists; use a new graph root");
    for (const auto& child : {path, path / "metadata", path / "adjacency"})
      if (std::filesystem::is_symlink(child))
        throw std::invalid_argument("Symlink graph path");
    rocksdb::Options options;
    options.create_if_missing = fresh || bool(cloud);
    rocksdb::DB* raw = nullptr;
    checked(rocksdb::DB::Open(options, (path / "metadata").string(), &raw));
    metadata.reset(raw);
    if (cloud) cloud->restore(*metadata);
    if (!fresh) {
      std::string format, state;
      checked(metadata->Get({}, "format", &format));
      checked(metadata->Get({}, "state", &state));
      if (format != "bw-jni-1" || state != "clean")
        throw std::runtime_error("Unsupported or unclean graph; recovery is not implemented");
    }
    if (cloud) cloud->mark_open();
    store = std::make_unique<multiscale_store_t>(path / "adjacency", fresh, true, "", budgets(), 1,
                                                 2, fresh);
    if (!fresh) {
      std::unique_ptr<rocksdb::Iterator> it(metadata->NewIterator({}));
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        auto k = it->key();
        if (!k.empty() && (k[0] == 'V' || k[0] == 'E')) {
          const auto id = decode(k);
          auto& mapper = k[0] == 'V' ? vertices : edges;
          mapper.insert(it->value().ToString(), id);
        }
      }
      checked(it->status());
      size_t live = 0;
      for (v_id_t v = 0; v < store->vertex_count(); ++v)
        if (!store->location(v).deleted) {
          vertex(v);
          ++live;
        }
      if (live != vertices.reverse.size())
        throw std::runtime_error("Vertex mapping mismatch");
      for (const auto& [id, outer] : edges.reverse) {
        (void) outer;
        vertex(id >> 32);
        vertex(uint32_t(id));
      }
    }
    rocksdb::WriteBatch batch;
    batch.Put("format", "bw-jni-1");
    batch.Put("state", "open");
    rocksdb::WriteOptions writes;
    writes.sync = true;
    checked(metadata->Write(writes, &batch));
  }
  ~graph_t() {
    try {
      close();
    } catch (...) {
    }
  }
  void healthy() const {
    if (closed || poisoned)
      throw std::runtime_error("Closed graph or failed mutation; close required");
  }
  void vertex(uint64_t v) const {
    if (v >= UINT32_MAX || !vertices.reverse.contains(v))
      throw std::out_of_range("Unknown vertex");
    if (v >= store->vertex_count() || store->location(v).deleted)
      throw std::runtime_error("Vertex mapping mismatch");
  }
  void entity(bool edge, uint64_t id) const {
    if (edge) {
      if (!edges.reverse.contains(id))
        throw std::out_of_range("Unknown edge");
    } else
      vertex(id);
  }
  void close() {
    if (closed)
      return;
    // A poisoned graph stays marked unclean, rather than silently accepting partial writes.
    try {
      store->close();
      if (!poisoned) {
        rocksdb::WriteOptions writes;
        writes.sync = true;
        checked(metadata->Put(writes, "state", "clean"));
        if (cloud) cloud->finish(*metadata);
      }
    } catch (...) {
      poisoned = true; // A failed close must not admit new storage operations.
      throw;
    }
    closed = true;
    metadata.reset();
    store.reset();
  }
  void erase_edge(uint64_t edge, rocksdb::WriteBatch& batch) {
    if (!store->update(edge >> 32, uint32_t(edge), DELETE))
      throw std::runtime_error("Edge deletion failed");
    batch.Delete(key('E', edge));
    batch.Delete(key('Q', edge));
    edges.erase(edge);
  }
  std::vector<uint64_t> incident(uint64_t v, int direction) {
    vertex(v);
    if (direction < 0 || direction > 2)
      throw std::invalid_argument("Direction must be OUT=0, IN=1 or BOTH=2");
    std::vector<uint64_t> result;
    for (int d = 0; d < 2; ++d)
      if (direction == d || direction == 2) {
        for (auto u : store->clone(v, d ? direction_t::in : direction_t::out)) {
          const uint64_t edge = d ? (uint64_t(u) << 32) | v : (v << 32) | u;
          if (!edges.reverse.contains(edge))
            throw std::runtime_error("Edge mapping mismatch");
          result.push_back(edge);
        }
      }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
  }
};
} // namespace
struct graph_service_t::impl_t {
  std::mutex mutex; // Only graph open/close; point requests do not take it.
  uint64_t next{1};
  folly::ConcurrentHashMap<uint64_t, std::shared_ptr<graph_t>> handles;
  folly::F14FastMap<std::string, std::shared_ptr<graph_t>> paths;
  std::shared_ptr<graph_t> get(uint64_t handle) {
    auto it = handles.find(handle);
    if (it == handles.end())
      throw std::invalid_argument("Invalid graph handle");
    return it->second;
  }
};
graph_service_t::graph_service_t() : impl_(std::make_unique<impl_t>()) {}
graph_service_t::~graph_service_t() = default;
uint64_t graph_service_t::open(const std::filesystem::path& root, const std::string& name) {
  if (name.empty() || name.size() > 128 ||
      !std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
      }))
    throw std::invalid_argument("Graph name must contain only ASCII letters, digits, _ or -");
  if (root.empty())
    throw std::invalid_argument("Empty graph root");
  std::filesystem::create_directories(root);
  const auto path = (std::filesystem::canonical(root) / name).string();
  std::lock_guard lock(impl_->mutex);
  if (impl_->next > INT64_MAX)
    throw std::overflow_error("Graph handles exhausted");
  auto& graph = impl_->paths[path];
  if (!graph)
    graph = std::make_shared<graph_t>(path);
  const auto handle = impl_->next++;
  impl_->handles.emplace(handle, graph);
  ++graph->leases;
  return handle;
}
bool graph_service_t::newly_created(uint64_t graph) { return impl_->get(graph)->created; }
void graph_service_t::reset(const std::filesystem::path& root, const std::string& name) {
  if (!s3_enabled()) return;
  if (name.empty() || name.size() > 128 || name.find_first_not_of(
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
    throw std::invalid_argument("Invalid graph name");
  const auto path = std::filesystem::weakly_canonical(root) / name;
  std::lock_guard registry(impl_->mutex);
  if (impl_->paths.contains(path.string())) throw std::runtime_error("Close graph before reset");
  s3_graph_t::reset(path);
  if (std::filesystem::exists(path)) {
    const auto backup = path.parent_path() / "backups" / (name + "-s3-" + s3_store_t().unique_id());
    std::filesystem::create_directories(backup.parent_path());
    std::filesystem::rename(path, backup);
  }
}
void graph_service_t::close(uint64_t handle) {
  std::lock_guard registry(impl_->mutex);
  auto it = impl_->handles.find(handle);
  if (it == impl_->handles.end())
    throw std::invalid_argument("Invalid graph handle");
  auto graph = it->second;
  if (graph->leases == 1) {
    std::unique_lock lock(graph->mutex);
    graph->close();
    for (auto p = impl_->paths.begin(); p != impl_->paths.end(); ++p)
      if (p->second == graph) {
        impl_->paths.erase(p);
        break;
      }
  }
  --graph->leases;
  impl_->handles.erase(handle);
}
uint64_t graph_service_t::create_vertex(uint64_t h, const std::string& id,
                                        const std::string& data) {
  auto g = impl_->get(h);
  std::unique_lock lock(g->mutex);
  g->healthy();
  if (g->vertices.forward.contains(id))
    throw std::invalid_argument("Duplicate vertex ID");
  try {
    const auto v = g->store->append_vertex();
    rocksdb::WriteBatch batch;
    batch.Put(key('V', v), id);
    batch.Put(key('P', v), data);
    checked(g->metadata->Write({}, &batch));
    g->vertices.insert(id, v); // Forward mapping is the final publication.
    return v;
  } catch (...) {
    g->poisoned = true;
    throw;
  }
}
uint64_t graph_service_t::create_edge(uint64_t h, const std::string& id, uint64_t src, uint64_t dst,
                                      const std::string& data) {
  auto g = impl_->get(h);
  std::shared_lock lock(g->mutex);
  auto stripes =
      g->write_stripes({g->external_stripe(true, id), g->entity_stripe(false, src),
                        g->entity_stripe(false, dst), g->entity_stripe(true, (src << 32) | dst)});
  g->healthy();
  g->vertex(src);
  g->vertex(dst);
  const auto edge = (src << 32) | dst;
  if (g->edges.forward.contains(id) || g->edges.reverse.contains(edge))
    throw std::invalid_argument("Duplicate edge ID or parallel edge");
  try {
    if (!g->store->update(src, dst, INSERT))
      throw std::runtime_error("Edge insertion failed");
    rocksdb::WriteBatch batch;
    batch.Put(key('E', edge), id);
    batch.Put(key('Q', edge), data);
    checked(g->metadata->Write({}, &batch));
    g->edges.insert(id, edge); // Publish only after topology and property completion.
    return edge;
  } catch (...) {
    g->poisoned = true;
    throw;
  }
}
int64_t graph_service_t::find(uint64_t h, bool edge, const std::string& id) {
  // The registry iterator hazard-protects its owning shared_ptr, eliminating a
  // per-request reference-count write. It must outlive the ID-map iterator.
  auto lease = impl_->handles.find(h);
  if (lease == impl_->handles.end())
    throw std::invalid_argument("Invalid graph handle");
  auto& g = *lease->second;
  g.healthy();
  const auto& map = edge ? g.edges.forward : g.vertices.forward;
  auto it = map.find(id);
  // Does not touch store/metadata, which close may already have released.
  // Forward entries are published last and never modified in place.
  const auto result = it == map.end() ? -1 : std::bit_cast<int64_t>(it->second);
  g.healthy();
  return result;
}

std::string graph_service_t::external_id(uint64_t h, bool edge, uint64_t id) {
  auto g = impl_->get(h);
  std::shared_lock lock(g->mutex);
  std::shared_lock stripe(g->stripes[g->entity_stripe(edge, id)]);
  g->healthy();
  g->entity(edge, id);
  return (edge ? g->edges : g->vertices).reverse.at(id);
}
std::string graph_service_t::get(uint64_t h, bool edge, uint64_t id) {
  auto g = impl_->get(h);
  std::shared_lock lock(g->mutex);
  std::shared_lock stripe(g->stripes[g->entity_stripe(edge, id)]);
  g->healthy();
  g->entity(edge, id);
  std::string result;
  checked(g->metadata->Get({}, key(edge ? 'Q' : 'P', id), &result));
  return result;
}
void graph_service_t::put(uint64_t h, bool edge, uint64_t id, const std::string& data) {
  auto g = impl_->get(h);
  std::shared_lock lock(g->mutex);
  auto stripe = g->write_stripes({g->entity_stripe(edge, id)});
  g->healthy();
  g->entity(edge, id);
  checked(g->metadata->Put({}, key(edge ? 'Q' : 'P', id), data));
}
void graph_service_t::remove(uint64_t h, bool edge, uint64_t id) {
  auto g = impl_->get(h);
  std::unique_lock lock(g->mutex);
  g->healthy();
  g->entity(edge, id);
  try {
    rocksdb::WriteBatch batch;
    if (edge)
      g->erase_edge(id, batch);
    else {
      for (auto e : g->incident(id, 2))
        g->erase_edge(e, batch);
      if (!g->store->delete_vertex(id))
        throw std::runtime_error("Vertex deletion failed");
      batch.Delete(key('V', id));
      batch.Delete(key('P', id));
      g->vertices.erase(id);
    }
    checked(g->metadata->Write({}, &batch));
  } catch (...) {
    g->poisoned = true;
    throw;
  }
}
std::vector<uint64_t> graph_service_t::all(uint64_t h, bool edge) {
  auto g = impl_->get(h);
  std::unique_lock lock(g->mutex);
  g->healthy();
  std::vector<uint64_t> result;
  for (const auto& [id, outer] : (edge ? g->edges : g->vertices).reverse) {
    (void) outer;
    result.push_back(id);
  }
  std::sort(result.begin(), result.end());
  return result;
}
std::vector<uint64_t> graph_service_t::incident(uint64_t h, uint64_t v, int d) {
  auto g = impl_->get(h);
  std::shared_lock lock(g->mutex);
  std::shared_lock stripe(g->stripes[g->entity_stripe(false, v)]);
  g->healthy();
  return g->incident(v, d);
}
std::vector<uint64_t> graph_service_t::bfs(uint64_t h, uint64_t source) {
  auto g = impl_->get(h);
  std::unique_lock lock(g->mutex);
  g->healthy();
  g->vertex(source);
  std::vector<uint64_t> result{source, 0};
  std::unordered_set<uint64_t> seen{source};
  for (size_t i = 0; i < result.size(); i += 2)
    for (auto v : g->store->clone(result[i]))
      if (seen.insert(v).second) {
        result.push_back(v);
        result.push_back(result[i + 1] + 1);
      }
  return result;
}
std::vector<uint64_t> graph_service_t::components(uint64_t h, bool strong) {
  auto g = impl_->get(h);
  std::unique_lock lock(g->mutex);
  g->healthy();
  std::vector<v_id_t> vertices;
  for (const auto& [id, outer] : g->vertices.reverse) {
    (void) outer;
    vertices.push_back(id);
  }
  std::sort(vertices.begin(), vertices.end());
  std::vector<v_id_t> order;
  std::unordered_set<v_id_t> seen;
  // Iterative DFS finish order; O(V+E) scratch only for SCC, not a persistent edge cache.
  if (strong)
    for (auto root : vertices)
      if (seen.insert(root).second) {
        struct frame_t {
          v_id_t v;
          std::vector<v_id_t> neighbors;
          size_t offset;
        };
        std::vector<frame_t> stack{{root, g->store->clone(root), 0}};
        while (!stack.empty()) {
          auto& top = stack.back();
          if (top.offset == top.neighbors.size()) {
            order.push_back(top.v);
            stack.pop_back();
            continue;
          }
          auto v = top.neighbors[top.offset++];
          if (seen.insert(v).second)
            stack.push_back({v, g->store->clone(v), 0});
        }
      }
  if (!strong)
    order = vertices;
  else
    std::reverse(order.begin(), order.end());
  seen.clear();
  std::vector<uint64_t> result{0};
  for (auto root : order)
    if (seen.insert(root).second) {
      const auto component = result[0]++;
      std::vector<v_id_t> queue{root};
      for (size_t i = 0; i < queue.size(); ++i) {
        auto v = queue[i];
        result.push_back(v);
        result.push_back(component);
        for (auto u : g->store->clone(v, direction_t::in))
          if (seen.insert(u).second)
            queue.push_back(u);
        if (!strong)
          for (auto u : g->store->clone(v))
            if (seen.insert(u).second)
              queue.push_back(u);
      }
    }
  return result;
}
std::vector<uint64_t> graph_service_t::communities(uint64_t h) {
  auto g = impl_->get(h);
  std::unique_lock lock(g->mutex);
  g->healthy();
  std::vector<v_id_t> live;
  for (const auto& [v, id] : g->vertices.reverse) {
    (void) id;
    live.push_back(v);
  }
  std::sort(live.begin(), live.end());
  if (live.empty())
    return {0};
  std::unordered_map<v_id_t, v_id_t> dense;
  for (size_t i = 0; i < live.size(); ++i)
    dense.emplace(live[i], i);
  std::vector<size_t> offsets(live.size() + 1);
  std::vector<v_id_t> edges;
  for (size_t i = 0; i < live.size(); ++i) {
    auto neighbors = g->store->clone(live[i]);
    auto incoming = g->store->clone(live[i], direction_t::in);
    neighbors.insert(neighbors.end(), incoming.begin(), incoming.end());
    std::sort(neighbors.begin(), neighbors.end());
    neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    for (auto v : neighbors)
      if (v != live[i])
        edges.push_back(dense.at(v));
    offsets[i + 1] = edges.size();
  }
  // Louvain runs on the simple undirected projection; stored edges remain directed.
  mem_sub_com_t projection(offsets, edges, std::vector<v_id_t>(live.size()));
  const auto partitions = compute_louvain_communities(projection);
  std::vector<uint64_t> result{partitions.size()};
  for (size_t c = 0; c < partitions.size(); ++c)
    for (auto v : partitions[c]) {
      result.push_back(live.at(v));
      result.push_back(c);
    }
  return result;
}
} // namespace bw_graph::service
