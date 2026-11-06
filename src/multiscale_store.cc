#include "bw_graph/db/multiscale_store.h"
#include "bw_graph/common/config.h"
#include "bw_graph/io/multiscale_csr_page_view.h"
#include "bw_graph/mem/mem_g0com.h"
#include "bw_graph/part/partition.h"
#include "bw_graph/part/louvain.h"
#include "bw_graph/part/reorder.h"
#include <bit>
#include "bw_graph/io/directional_csr_page.h"
#include <numeric>
#include <set>
#include <unordered_map>

namespace bw_graph {
namespace {
constexpr uint32_t index_magic = 0x31494742, delta_magic = 0x32444742;
constexpr uint32_t delta_header = 32;
#ifdef BW_GRAPH_ENABLE_TRANSACTION
constexpr uint32_t delta_record_bytes = 40, transaction_format = 1;
#else
constexpr uint32_t delta_record_bytes = 24, transaction_format = 0;
#endif
uint32_t u32(const std::byte* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t u64(const std::byte* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
void put32(std::byte* p, uint32_t v) { std::memcpy(p, &v, 4); }
void put64(std::byte* p, uint64_t v) { std::memcpy(p, &v, 8); }
multiscale_disk_options_t disk_options() {
  multiscale_disk_options_t o;
  o.index_page_size = 16384;
  return o;
}
}
neighbor_range_t::neighbor_range_t(neighbor_span_t span, csr_page_t* page)
    : legacy_page_(page), data_(reinterpret_cast<const std::byte*>(span.data())),
      count_(span.size()), remaining_(span.size()), size_(span.size()) {}
neighbor_range_t::neighbor_range_t(multiscale_store_t& store, const adjacency_t& adjacency)
    : store_(&store), adjacency_(&adjacency), remaining_(adjacency.degree),
      size_(adjacency.degree) {
  if (remaining_) load_next();
}
neighbor_range_t::~neighbor_range_t() { if (legacy_page_) legacy_page_->r_unlatch(); }
neighbor_range_t::neighbor_range_t(neighbor_range_t&& o) noexcept
    : store_(o.store_), adjacency_(o.adjacency_), version_(std::move(o.version_)),
      head_(o.head_), extent_consumed_(o.extent_consumed_),
      legacy_page_(std::exchange(o.legacy_page_, nullptr)), guard_(std::move(o.guard_)),
      data_(o.data_), position_(o.position_), count_(o.count_),
      remaining_(std::exchange(o.remaining_, 0)), size_(o.size_), extent_(o.extent_),
      page_in_extent_(o.page_in_extent_) {}
void neighbor_range_t::load_next() {
  guard_.reset();
  const auto& f = extent_ == 0 ? adjacency_->first : adjacency_->rest.at(extent_ - 1);
  auto id = page_id_t::make(f.page.space(), f.page.slot() + page_in_extent_);
  const auto c = static_cast<size_t>(id.space());
  const auto* resident = id.slot() < store_->resident_[c].size() ?
      store_->resident_[c][id.slot()] : nullptr;
  std::span<const std::byte> bytes;
  if (resident) {
    bytes = {resident, store_->disk().page_size(id.space())};
  } else {
    if (id.slot() < store_->resident_[c].size())
      guard_.emplace(store_->pool().fetch_read(id, store_->hints_[c][id.slot()]));
    else guard_.emplace(store_->pool().fetch_read(id));
    bytes = guard_->bytes();
    store_->validate_base(id, bytes);
  }
  if (f.byte_offset < 32 || f.byte_offset % 4 || f.byte_offset >= bytes.size())
    throw std::runtime_error("Invalid adjacency offset");
  // Persisted index addresses the immutable base directly; no repeated page parsing.
  count_ = std::min(remaining_, (bytes.size() - f.byte_offset) / 4);
  if (f.edges) count_ = std::min<uint64_t>(count_, f.edges - extent_consumed_);
  extent_consumed_ += count_;
  data_ = bytes.data() + f.byte_offset;
  position_ = 0;
  if (++page_in_extent_ == f.count) {
    ++extent_; page_in_extent_ = 0; extent_consumed_ = 0;
  }
}
multiscale_store_t::multiscale_store_t(const std::filesystem::path& path, bool rebuild,
    bool id_order, const std::string& graph_file, multiscale_buf_options_t options,
    size_t smo_workers, size_t delta_page_threshold, bool create_empty)
    : disk_(path, disk_options()), options_(options),
      delta_threshold_(std::max<size_t>(1, delta_page_threshold)), smo_enabled_(smo_workers != 0) {
  if (create_empty && (!options_.groups[0].chunk_count || !options_.groups[0].chunk_size ||
                       !options_.groups[5].chunk_count || !options_.groups[5].chunk_size))
    throw std::invalid_argument("Empty graph requires CSR 4K and Delta frame budgets");
  if constexpr (std::endian::native != std::endian::little)
    throw std::runtime_error("Multiscale graph format requires little endian");
  if (rebuild)
    for (size_t c = 0; c < 6; ++c)
      if (disk_.get_page_count(static_cast<page_space_t>(c)))
        throw std::runtime_error("Rebuild requires a new multiscale store directory");
  if (disk_.get_page_count(page_space_t::INDEX)) load_index();
  else {
    if (!rebuild) throw std::runtime_error("Multiscale graph index missing");
    if (create_empty) {
      tat_.build({}, [](auto) { return std::vector<v_id_t>{}; });
      index_dirty_ = true;
    }
    else build(graph_file, id_order);
  }
  published_vertices_ = vertices_.size();
  for (size_t c = 0; c < 4; ++c) {
    auto n = disk_.get_page_count(static_cast<page_space_t>(c));
    hints_[c] = std::make_unique<page_frame_hint_t[]>(n);
    resident_[c].resize(n, nullptr);
    // Empty stores retain configured frame budgets for incremental growth.
  }
  refresh_scan_order();
  // Index images are already fully resident; no duplicate Index frames in this pool.
  options_.groups[4] = {0, 0};
  pool_ = std::make_unique<multiscale_buf_pool_t>(disk_, options_);
  initialize_writers();
  checkpoint();
  for (size_t i = 0; i < smo_workers; ++i) workers_.emplace_back([this] { worker_loop(); });
}
void multiscale_store_t::refresh_scan_order() {
  scan_order_.clear(); scan_groups_.clear();
  scan_order_.reserve(vertex_count());
  uint64_t edges = 0;
  for (const auto& leaf : tat_.leaves()) {
    const auto start = scan_order_.size();
    for (auto v : leaf.vertices) {
      const auto& root = vertices_[v];
      auto version = std::atomic_load(&root.version);
      if (!root.deleted) {
        const auto degree = version ? version->degree : root.degree;
        edges += degree;
        if (degree) scan_order_.push_back(v);
      }
    }
    if (scan_order_.size() != start) scan_groups_.push_back(start);
  }
  scan_groups_.push_back(scan_order_.size());
  base_edges_ = edges;
}
multiscale_store_t::~multiscale_store_t() {
  try { close(); } catch (...) {
    { std::lock_guard lock(queue_mutex_); stopping_ = true; }
    queue_cv_.notify_all();
    for (auto& thread : workers_) if (thread.joinable()) thread.join();
  }
}
const adjacency_t& multiscale_store_t::location(v_id_t v) const {
  if (v >= vertex_count()) throw std::out_of_range("Vertex ID");
  return vertices_[v];
}
neighbor_range_t multiscale_store_t::read_neighbor(v_id_t v, direction_t direction) {
  if (closed_) throw std::runtime_error("Graph store closed");
  const auto& a = location(v);
  if (a.deleted.load(std::memory_order_acquire)) return {};
  a.reads.fetch_add(1, std::memory_order_relaxed);
  for (;;) {
    auto version = std::atomic_load_explicit(&a.version, std::memory_order_acquire);
    auto head = (direction == direction_t::out ? a.head : a.in_head).load(std::memory_order_acquire);
    if (version != std::atomic_load_explicit(&a.version, std::memory_order_acquire)) continue;
    const auto& base = version ? *version : a;
    const auto& selected = direction == direction_t::out ? base : incoming_base(base);
    neighbor_range_t result(*this, selected);
    result.version_ = direction == direction_t::out ? std::move(version) : base.incoming;
    result.head_ = head;
    return result;
  }
}
uint64_t multiscale_store_t::delta_head(v_id_t v) const {
  const auto& a = location(v);
  return a.deleted.load(std::memory_order_acquire) ? UINT64_MAX :
      a.head.load(std::memory_order_acquire);
}
void multiscale_store_t::validate_base(page_id_t id, std::span<const std::byte> bytes) {
  if (bytes.size() < 32 || u32(bytes.data() + 12) != id.raw())
    throw std::runtime_error("Invalid CSR page identity");
  if (u32(bytes.data()) == directional_csr_page_t::magic) {
    directional_csr_page_t::validate_header(bytes, id.raw());
    return;
  }
  if (u32(bytes.data()) == 0x3150434d) {
    (void)multiscale_csr_page_view_t(bytes); // Header-only validation; no decode/copy.
  } else if (u32(bytes.data()) == 0x3350434d) {
    if (bytes.size() < 40 || u32(bytes.data() + 8) != bytes.size() ||
        u32(bytes.data() + 24) > bytes.size() || u32(bytes.data() + 24) < 40)
      throw std::runtime_error("Invalid timestamped CSR page");
  } else if (u32(bytes.data()) == 0x3250434d) {
    const auto used = u32(bytes.data() + 24);
    if (id.space() != page_space_t::CSR_4K || bytes.size() != 4096 ||
        u32(bytes.data() + 4) != 1 || u32(bytes.data() + 8) != 4096 ||
        u32(bytes.data() + 28) || used < 32 || used > 4096 || used % 4 ||
        u32(bytes.data() + 20) != (used - 32) / 4)
      throw std::runtime_error("Invalid packed CSR header");
  } else {
    throw std::runtime_error("Unknown CSR page format");
  }
}
void multiscale_store_t::build(const std::string& file, bool id_order) {
  mem_sub_com_t graph(file, true, LOAD_BUFFER_SIZE, true, true);
  vertices_.grow_to_at_least(graph.vertex_count());
  base_edges_ = graph.edge_count();
  std::vector<std::vector<v_id_t>> incoming(graph.vertex_count());
  for (v_id_t v = 0; v < graph.vertex_count(); ++v)
    for (auto dst : graph.get_neighbors_span(v)) incoming[dst].push_back(v);
  graph_partition_t partitions;
  std::unique_ptr<mem_sub_com_t> projection;
  if (id_order) {
    partitions.resize(1); partitions[0].resize(graph.vertex_count());
    std::iota(partitions[0].begin(), partitions[0].end(), 0);
  } else {
    // Partition/reordering use the undirected projection, while persisted edges
    // retain their original orientation. This does not create logical reverse edges.
    std::vector<size_t> offsets(graph.vertex_count() + 1);
    std::vector<v_id_t> edges;
    for (v_id_t v = 0; v < graph.vertex_count(); ++v) {
      auto out = graph.get_neighbors_span(v);
      std::vector<v_id_t> neighbors(out.begin(), out.end());
      neighbors.insert(neighbors.end(), incoming[v].begin(), incoming[v].end());
      std::sort(neighbors.begin(), neighbors.end());
      neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
      std::erase(neighbors, v); // Partitioners receive a loop-free undirected projection.
      edges.insert(edges.end(), neighbors.begin(), neighbors.end()); offsets[v + 1] = edges.size();
    }
    projection = std::make_unique<mem_sub_com_t>(offsets, edges,
                                                 std::vector<comm_id_t>(graph.vertex_count()));
    if (BW_GRAPH_LAYOUT == graph_layout_t::louvain_gorder)
      partitions = compute_louvain_communities(*projection);
    else partitions = compute_absolute_weight_graph_partition_non_giant(*projection,
                                                                         BW_GRAPH_COARSENING_WEIGHT);
  }
  using input_t = directional_csr_page_t::input_t;
  std::vector<multiscale_tat_t::leaf_t> leaves;
  std::vector<input_t> small;
  size_t used = 40;
  auto flush_small = [&] {
    if (small.empty()) return;
    auto id = disk_.allocate_page(page_space_t::CSR_4K);
    auto bytes = directional_csr_page_t::build(4096, id.raw(), small);
    multiscale_tat_t::leaf_t leaf{id, {}};
    uint32_t offset = 40 + small.size() * 32;
    for (const auto& input : small) {
      auto& a = vertices_[input.vertex_id];
      a.anchor = id; a.leaf = id; a.incoming->anchor = id; a.incoming->leaf = id;
      leaf.vertices.push_back(input.vertex_id);
      if (!input.neighbors.empty()) a.first = {id, offset, 1, input.neighbors.size()};
      offset += input.neighbors.size_bytes();
      if (!input.incoming.empty()) a.incoming->first = {id, offset, 1, input.incoming.size()};
      offset += input.incoming.size_bytes();
    }
    leaves.push_back(std::move(leaf)); disk_.write_page(id, bytes);
    small.clear(); used = 40;
  };
  auto extent = [&](v_id_t v, std::span<const v_id_t> neighbors, adjacency_t& a, direction_t d) {
    size_t offset = 0;
    for (int c = 3; c >= 0 && offset < neighbors.size(); --c) {
      auto space = static_cast<page_space_t>(c);
      const size_t capacity = (disk_.page_size(space) - 72) / 4;
      size_t count = (neighbors.size() - offset) / capacity;
      if (!c && (neighbors.size() - offset) % capacity) ++count;
      if (!count) continue;
      auto first = disk_.allocate_pages(space, count);
      neighbor_fragment_t fragment{first, 72, uint32_t(count),
                                    std::min(neighbors.size() - offset, count * capacity)};
      if (!a.first.count) a.first = fragment; else a.rest.push_back(fragment);
      for (size_t p = 0; p < count; ++p) {
        auto id = page_id_t::make(space, first.slot() + p);
        auto n = std::min(capacity, neighbors.size() - offset);
        input_t input{v, {}, 0, {}};
        (d == direction_t::out ? input.neighbors : input.incoming) = neighbors.subspan(offset, n);
        disk_.write_page(id, directional_csr_page_t::build(disk_.page_size(space), id.raw(), {&input, 1}));
        offset += n;
      }
    }
  };
  for (auto& part : partitions) {
    if (!id_order) {
      folly::F14FastSet<v_id_t> members(part.begin(), part.end());
      part = gorder_reordering_optimized(projection->induce_subgraph_filter(members));
    }
    for (auto v : part) {
      auto out = graph.get_neighbors_span(v);
      std::span<const v_id_t> in(incoming[v]);
      auto& a = vertices_[v]; a.degree = out.size();
      a.incoming = std::make_shared<adjacency_t>(); a.incoming->degree = in.size();
      bool inline_out = out.size_bytes() + 72 <= 4096;
      bool inline_in = in.size_bytes() + 72 <= 4096;
      if (inline_out && inline_in && out.size_bytes() + in.size_bytes() + 72 > 4096) {
        if (out.size() >= in.size()) inline_out = false; else inline_in = false;
      }
      if (!inline_out) extent(v, out, a, direction_t::out);
      if (!inline_in) extent(v, in, *a.incoming, direction_t::in);
      input_t input{v, inline_out ? out : std::span<const v_id_t>{}, 0,
                       inline_in ? in : std::span<const v_id_t>{}};
      const auto bytes = 32 + input.neighbors.size_bytes() + input.incoming.size_bytes();
      if (used + bytes > 4096) flush_small();
      small.push_back(input); used += bytes;
    }
  }
  flush_small(); tat_.build(std::move(leaves), {}); index_dirty_ = true;
}
bool multiscale_store_t::base_fits() const {
  for (size_t c = 0; c < 4; ++c)
    if (disk_.get_page_count(static_cast<page_space_t>(c)) >
        options_.groups[c].chunk_count * options_.groups[c].chunk_size) return false;
  return true;
}
bool multiscale_store_t::delete_vertex(v_id_t v) {
  std::unique_lock topology(topology_mutex_);
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  std::lock_guard transaction_lock(transaction_mutex_);
  if (!snapshots_.empty())
    throw std::logic_error("Vertex lifetime changes require quiescent native snapshots");
#endif
  if (v >= vertex_count()) return false;
  std::lock_guard lock(writer(v).mutex);
  if (vertices_[v].deleted.exchange(true, std::memory_order_acq_rel)) return false;
  index_dirty_ = true;
  return true;
}
bool multiscale_store_t::update(v_id_t src, v_id_t dst, delta_type_t type) {
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  std::vector<write_opt_t> writes{{type == INSERT ? write_ops_type_t::INSERT_EDGE :
      write_ops_type_t::DELETE_EDGE, src, dst}};
  return commit_edges(UINT64_MAX, writes);
#else
  if (src >= vertex_count() || dst >= vertex_count()) return false;
  if (vertices_[src].unattached.load() || vertices_[dst].unattached.load()) {
    std::unique_lock topology(topology_mutex_);
    if (vertices_[src].deleted || vertices_[dst].deleted) return false;
    orphan_page_ = {};
    if (type == INSERT) attach_locked(src, {&dst, 1});
    return update_pair(src, dst, type);
  }
  std::shared_lock topology(topology_mutex_);
  return update_pair(src, dst, type);
#endif
}
bool multiscale_store_t::update_pair(v_id_t src, v_id_t dst, delta_type_t type,
                                    uint64_t timestamp, uint64_t begin_timestamp) {
  if (src >= vertex_count() || dst >= vertex_count()) return false;
  const auto a = vertices_[src].anchor.slot(), b = vertices_[dst].anchor.slot();
  std::unique_lock first(writers_[std::min(a, b)]->mutex);
  std::unique_lock<std::mutex> second;
  if (a != b) second = std::unique_lock(writers_[std::max(a, b)]->mutex);
  if (vertices_[src].deleted || vertices_[dst].deleted) return false;
  const auto out_head = vertices_[src].head.load(), in_head = vertices_[dst].in_head.load();
  try {
    update_record(src, dst, type, timestamp, begin_timestamp, direction_t::out);
    update_record(dst, src, type, timestamp, begin_timestamp, direction_t::in);
  } catch (...) {
    vertices_[src].head.store(out_head, std::memory_order_release);
    vertices_[dst].in_head.store(in_head, std::memory_order_release);
    throw;
  }
  if (type == INSERT) { vertices_[src].unattached = false; vertices_[dst].unattached = false; }
  return true;
}
bool multiscale_store_t::update_record(v_id_t src, v_id_t dst, delta_type_t type,
                                       [[maybe_unused]] uint64_t timestamp,
                                       [[maybe_unused]] uint64_t begin_timestamp, direction_t direction) {
  if (src >= vertex_count() || dst >= vertex_count()) return false;
  auto& writer = this->writer(src);
  // update_pair owns both endpoint writer locks.
  auto& vertex = vertices_[src];
  if (vertex.deleted || vertices_[dst].deleted) return false;
  const uint32_t capacity = (disk_.page_size(page_space_t::DELTA) - delta_header) /
                            delta_record_bytes;
  const bool fresh_page = !writer.page.valid() || writer.used == capacity;
  if (fresh_page) {
    writer.page = allocate_runtime_page(page_space_t::DELTA);
    writer.used = 0;
  }
  auto& delta_head = direction == direction_t::out ? vertex.head : vertex.in_head;
  uint64_t head = delta_head.load(std::memory_order_relaxed);
  const uint64_t pointer = (uint64_t(writer.page.raw()) << 32) | writer.used;
  {
    auto guard = fresh_page ? pool_->new_page(writer.page) :
        pool_->fetch_write(writer.page, writer.hint);
    auto bytes = guard.bytes();
    if (!writer.used) {
      std::fill(bytes.begin(), bytes.end(), std::byte{0});
      put32(bytes.data(), delta_magic);
      put32(bytes.data() + 8, vertex.anchor.raw());
      put32(bytes.data() + 12, writer.chain.raw());
      put32(bytes.data() + 16, writer.page.raw());
      writer.chain = writer.page;
    }
    auto* rec = bytes.data() + delta_header + writer.used * delta_record_bytes;
    put32(rec, src); put32(rec + 4, dst); put32(rec + 8, static_cast<uint32_t>(type));
    put32(rec + 12, static_cast<uint32_t>(direction));
    put64(rec + 16, head);
#ifdef BW_GRAPH_ENABLE_TRANSACTION
    put64(rec + 24, begin_timestamp); put64(rec + 32, timestamp);
#endif
    put32(bytes.data() + 4, ++writer.used);
  } // Publish the immutable record only after releasing the write guard.
  delta_head.store(pointer, std::memory_order_release);
  // Checkpoint requires quiescent producers. Once dirty, avoid transferring a
  // global cache line between writers on every edge; head/guard publish data.
  if (!index_dirty_.load(std::memory_order_relaxed))
    index_dirty_.store(true, std::memory_order_release);
  ++writer.pending;
  if (smo_enabled_ && writer.pending >= (delta_threshold_ - 1) * capacity + 1)
    queue_smo(vertex.anchor.slot(), writer);
  return true;
}
directed_delta_record_t multiscale_store_t::read_delta(uint64_t pointer) {
  const auto id = page_id_t::from_raw(pointer >> 32);
  if (id.space() != page_space_t::DELTA) throw std::runtime_error("Invalid Delta pointer");
  auto guard = pool_->fetch_read(id);
  return decode_delta(pointer, guard.bytes());
}
directed_delta_record_t multiscale_store_t::decode_delta(uint64_t pointer, std::span<const std::byte> bytes) {
  const uint32_t slot = pointer;
  if (u32(bytes.data()) != delta_magic || u32(bytes.data() + 16) != uint32_t(pointer >> 32) ||
      slot >= u32(bytes.data() + 4) ||
      uint64_t(delta_header) + uint64_t(slot + 1) * delta_record_bytes > bytes.size())
    throw std::runtime_error("Invalid Delta record");
  auto* rec = bytes.data() + delta_header + slot * delta_record_bytes;
  directed_delta_record_t result;
  if (u32(rec + 12) > 1) throw std::runtime_error("Invalid Delta direction");
  result.direction = static_cast<direction_t>(u32(rec + 12));
  result.target = EDGE; result.first = u32(rec); result.second = u32(rec + 4);
  const auto type = u32(rec + 8);
  if (type != INSERT && type != DELETE) throw std::runtime_error("Invalid Delta operation");
  result.type = static_cast<delta_type_t>(type);
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  result.begin_ts = u64(rec + 24); result.commit_ts = u64(rec + 32);
#endif
  auto next = u64(rec + 16);
  if (next != UINT64_MAX && next >= pointer) throw std::runtime_error("Cyclic Delta link");
  result.next_page_no = next >> 32; result.next_record_idx = next;
  return result;
}
std::vector<v_id_t> multiscale_store_t::clone(v_id_t v, direction_t direction) {
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  auto ts = begin_snapshot();
  try { auto result = clone_at(v, ts, direction); end_snapshot(ts); return result; }
  catch (...) { end_snapshot(ts); throw; }
#else
  return clone_at(v, UINT64_MAX, direction);
#endif
}
std::vector<v_id_t> multiscale_store_t::clone_at(v_id_t v, [[maybe_unused]] uint64_t ts, direction_t direction) {
  if (location(v).deleted) return {};
  auto base = read_neighbor(v, direction);
  if (ts < base.base_timestamp()) throw std::logic_error("Snapshot predates base watermark");
  auto head = base.delta_head();
  const auto stop = base.delta_stop();
  std::unordered_map<v_id_t, delta_type_t> latest;
  while (head != UINT64_MAX && head != stop) {
    auto rec = read_delta(head);
    if (rec.first != v || rec.direction != direction) throw std::runtime_error("Delta chain direction mismatch");
#ifdef BW_GRAPH_ENABLE_TRANSACTION
    if (uint64_t(rec.commit_ts) <= ts)
#endif
      latest.try_emplace(rec.second, rec.type);
    head = (uint64_t(rec.next_page_no) << 32) | rec.next_record_idx;
  }
  std::vector<v_id_t> result;
  result.reserve(base.size() + latest.size());
  for (auto dst : base) if (!latest.contains(dst)) result.push_back(dst);
  for (auto [dst, op] : latest) if (op == INSERT) result.push_back(dst);
  return result;
}
std::vector<v_id_t> multiscale_store_t::topology_edges(v_id_t v) {
  auto& root = vertices_[v];
  auto version = std::atomic_load_explicit(&root.version, std::memory_order_acquire);
  neighbor_range_t range(*this, version ? *version : root);
  std::vector<v_id_t> result;
  result.reserve(range.size());
  for (auto dst : range) result.push_back(dst);
  return result;
}
uint64_t multiscale_store_t::degree(v_id_t v) {
  if (location(v).deleted.load(std::memory_order_acquire)) return 0;
  return clone(v).size();
}
std::vector<std::byte> multiscale_store_t::encode_index() const {
  // Checkpoint is quiescent: reading published shared_ptr objects directly is
  // safe after wait_for_smo(), avoiding an atomic shared_ptr lock per vertex.
  std::vector<std::byte> bytes(48);
  bytes.reserve(48 + vertices_.size() * 32);
  put32(bytes.data(), index_magic); put32(bytes.data() + 4, 8);
  put64(bytes.data() + 8, vertices_.size()); put64(bytes.data() + 16, base_edges_);
  put64(bytes.data() + 32, committed_ts_.load());
  put32(bytes.data() + 40, transaction_format);
  for (const auto& root : vertices_) {
    const auto& outer = root.version ? *root.version : root;
    for (auto direction : {direction_t::out, direction_t::in}) {
    const auto& a = direction == direction_t::out ? outer : incoming_base(outer);
    const uint64_t head = direction == direction_t::out ? root.head.load() : root.in_head.load();
    const bool delta = head != UINT64_MAX && head != a.stop;
    const uint32_t flags = uint32_t(root.deleted) | (delta ? 2u : 0u) | (a.base_ts ? 4u : 0u) | (root.unattached ? 8u : 0u);
    const uint32_t count = a.first.count ? 1 + a.rest.size() : 0;
    const size_t offset = bytes.size();
    bytes.resize(offset + 20 + (delta ? 16 : 0) + (a.base_ts ? 8 : 0) + count * 16);
    auto* p = bytes.data() + offset;
    if (a.degree > UINT32_MAX) throw std::overflow_error("Vertex degree exceeds 32-bit ID domain");
    put32(p, a.degree); put32(p + 4, root.anchor.raw());
    put32(p + 8, flags); put32(p + 12, count); put32(p + 16, outer.leaf.raw()); p += 20;
    if (delta) { put64(p, head); put64(p + 8, a.stop); p += 16; }
    if (a.base_ts) { put64(p, a.base_ts); p += 8; }
    for (uint32_t i = 0; i < count; ++i, p += 16) {
      const auto& f = i ? a.rest[i - 1] : a.first;
      if (f.edges > UINT32_MAX) throw std::overflow_error("Extent edge count");
      put32(p, f.page.raw()); put32(p + 4, f.byte_offset);
      put32(p + 8, f.count); put32(p + 12, f.edges);
    }
  }
  }
  tat_.encode(bytes);
  put64(bytes.data() + 24, bytes.size());
  return bytes;
}
void multiscale_store_t::load_index() {
  auto count = disk_.get_page_count(page_space_t::INDEX);
  std::vector<std::byte> bytes;
  bytes.reserve(count * disk_.page_size(page_space_t::INDEX));
  for (uint32_t i = 0; i < count; ++i) {
    index_images_.push_back(disk_.read_page(page_id_t::make(page_space_t::INDEX, i)));
    auto b = index_images_.back().bytes(); bytes.insert(bytes.end(), b.begin(), b.end());
  }
  if (bytes.size() < 48 || u32(bytes.data()) != index_magic || u32(bytes.data() + 4) != 8)
    throw std::runtime_error("Invalid graph index header (fresh directed format-8 store required)");
  if (u32(bytes.data() + 40) != transaction_format)
    throw std::runtime_error("Graph transaction format differs from build configuration");
  auto n = u64(bytes.data() + 8), used = u64(bytes.data() + 24);
  if (used > bytes.size() || used < 48 || n > UINT32_MAX || n > (used - 48) / 16)
    throw std::runtime_error("Invalid graph index size");
  vertices_.grow_to_at_least(n); base_edges_ = u64(bytes.data() + 16);
  committed_ts_ = u64(bytes.data() + 32);
  size_t offset = 48;
  auto valid_delta = [&](uint64_t pointer) {
    if (pointer == UINT64_MAX) return;
    const auto id = page_id_t::from_raw(pointer >> 32);
    if (id.space() != page_space_t::DELTA || id.slot() >= disk_.get_page_count(id.space()) ||
        uint32_t(pointer) >= (disk_.page_size(id.space()) - delta_header) / delta_record_bytes)
      throw std::runtime_error("Invalid persisted Delta pointer");
  };
  for (auto& root : vertices_) {
    root.incoming = std::make_shared<adjacency_t>();
    for (auto direction : {direction_t::out, direction_t::in}) {
    auto& a = direction == direction_t::out ? root : *root.incoming;
    if (offset + 20 > used) throw std::runtime_error("Short vertex index");
    auto* p = bytes.data() + offset;
    a.degree = u32(p); a.anchor = page_id_t::from_raw(u32(p + 4));
    const auto flags = u32(p + 8), count = u32(p + 12);
    if (flags & ~15u) throw std::runtime_error("Invalid vertex flags");
    a.leaf = page_id_t::from_raw(u32(p + 16));
    a.unattached = flags & 8; a.deleted = flags & 1; offset += 20; p += 20;
    const uint64_t extra = (flags & 2 ? 16 : 0) + (flags & 4 ? 8 : 0) + uint64_t(count) * 16;
    if (offset + extra > used) throw std::runtime_error("Short extent index");
    if (flags & 2) { a.head = u64(p); a.stop = u64(p + 8); p += 16; }
    if (flags & 4) { a.base_ts = u64(p); p += 8; }
    valid_delta(a.head); valid_delta(a.stop);
    uint64_t capacity = 0;
    for (uint32_t i = 0; i < count; ++i, p += 16) {
      neighbor_fragment_t f{page_id_t::from_raw(u32(p)), u32(p + 4), u32(p + 8), u32(p + 12)};
      if (static_cast<unsigned>(f.page.space()) >= 4 || !f.count ||
          uint64_t(f.page.slot()) + f.count > disk_.get_page_count(f.page.space()))
        throw std::runtime_error("Invalid extent");
      const auto size = disk_.page_size(f.page.space());
      if (f.byte_offset < 32 || f.byte_offset % 4 || f.byte_offset >= size)
        throw std::runtime_error("Invalid persisted adjacency offset");
      const uint64_t per_page = (size - f.byte_offset) / 4;
      if (!f.edges || f.edges > uint64_t(f.count) * per_page ||
          f.edges <= uint64_t(f.count - 1) * per_page)
        throw std::runtime_error("Invalid extent edge count");
      capacity += f.edges;
      if (i) a.rest.push_back(f); else a.first = f;
    }
    if ((a.degree != 0) != (count != 0) || a.degree != capacity)
      throw std::runtime_error("Invalid base degree");
    offset += extra;
    }
    root.in_head = root.incoming->head.load();
    if (root.incoming->anchor != root.anchor || root.incoming->leaf != root.leaf)
      throw std::runtime_error("Incoming anchor mismatch");
  }
  tat_.decode(std::span<const std::byte>(bytes).subspan(offset, used - offset));
  size_t members = 0;
  for (const auto& leaf : tat_.leaves()) {
    if (leaf.page.slot() >= disk_.get_page_count(page_space_t::CSR_4K))
      throw std::runtime_error("TAT page outside store");
    for (auto v : leaf.vertices) {
      if (v >= vertices_.size() || vertices_[v].leaf != leaf.page)
        throw std::runtime_error("TAT/vertex index mismatch");
      ++members;
    }
  }
  if (members != vertices_.size()) throw std::runtime_error("Incomplete TAT membership");
}
void multiscale_store_t::checkpoint() {
  wait_for_smo();
  if (pool_) pool_->flush_all(); else disk_.flush();
  if (!index_dirty_.load(std::memory_order_acquire)) return;
  auto bytes = encode_index();
  const size_t size = disk_.page_size(page_space_t::INDEX);
  bytes.resize((bytes.size() + size - 1) / size * size);
  for (size_t i = 0; i < bytes.size() / size; ++i) {
    if (i == index_images_.size()) {
      auto id = disk_.allocate_page(page_space_t::INDEX);
      index_images_.emplace_back(id, size);
      std::fill(index_images_.back().bytes().begin(), index_images_.back().bytes().end(),
                std::byte{0});
    }
    auto& image = index_images_[i];
    auto source = std::span<const std::byte>(bytes).subspan(i * size, size);
    if (std::memcmp(image.bytes().data(), source.data(), size)) {
      disk_.write_page(image.page_id(), source);
      std::memcpy(image.bytes().data(), source.data(), size);
    }
  }
  disk_.flush(); index_dirty_ = false;
}
void multiscale_store_t::close() {
  if (closed_) return;
  wait_for_smo();
  { std::lock_guard lock(queue_mutex_); stopping_ = true; }
  queue_cv_.notify_all();
  for (auto& thread : workers_) if (thread.joinable()) thread.join();
  resident_pins_.clear();
  for (auto& group : resident_) std::fill(group.begin(), group.end(), nullptr);
  checkpoint();
  if (!pool_->close()) throw std::runtime_error("Release neighbor ranges before closing graph");
  disk_.shutdown(); closed_ = true;
}
uint64_t multiscale_store_t::index_bytes() const {
  uint64_t bytes = vertices_.capacity() * sizeof(adjacency_t) + vertices_.directory_bytes();
  for (const auto& a : vertices_) {
    bytes += a.rest.capacity() * sizeof(neighbor_fragment_t);
    auto version = std::atomic_load(&a.version);
    if (version) bytes += sizeof(adjacency_t) + version->rest.capacity() * sizeof(neighbor_fragment_t);
    if (a.incoming)
      bytes += sizeof(adjacency_t) + a.incoming->rest.capacity() * sizeof(neighbor_fragment_t);
    if (version && version->incoming && version->incoming != a.incoming)
      bytes += sizeof(adjacency_t) + version->incoming->rest.capacity() * sizeof(neighbor_fragment_t);
  }
  bytes += writers_.size() * sizeof(std::unique_ptr<delta_writer_t>);
  for (const auto& w : writers_) if (w)
    bytes += sizeof(delta_writer_t) + w->members.capacity() * sizeof(v_id_t);
  for (const auto& p : index_images_) bytes += p.bytes().size();
  for (size_t c = 0; c < 4; ++c)
    bytes += resident_[c].size() *
             (sizeof(page_frame_hint_t) + sizeof(const std::byte*));
  bytes += scan_order_.capacity() * sizeof(v_id_t) + scan_groups_.capacity() * sizeof(size_t);
  bytes += resident_pins_.capacity() * sizeof(page_pin_t);
  bytes += tat_.memory_bytes();
  return bytes;
}
uint64_t multiscale_store_t::giant_bytes() const {
  std::set<uint32_t> ids;
  for (const auto& root : vertices_) {
    auto version = std::atomic_load(&root.version);
    const auto& outer = version ? *version : root;
    for (const auto* entry : {&outer, &incoming_base(outer)}) {
    const auto& a = *entry;
    if (root.deleted || a.degree * 4 + 72 <= 4096) continue;
    auto add = [&](const neighbor_fragment_t& f) {
      for (uint32_t i = 0; i < f.count; ++i) ids.insert(f.page.raw() + i);
    };
    add(a.first); for (const auto& f : a.rest) add(f);
    }
  }
  uint64_t bytes = 0;
  for (auto id : ids) bytes += disk_.page_size(page_id_t::from_raw(id).space());
  return bytes;
}
void multiscale_store_t::warm_up(bool require_all_giants) {
  wait_for_smo();
  refresh_scan_order();
  resident_pins_.clear();
  for (auto& group : resident_) std::fill(group.begin(), group.end(), nullptr);
  std::array<std::set<uint32_t>, 4> giant, live;
  for (const auto& root : vertices_) {
    const auto& outer = root.version ? *root.version : root; // Quiescent warmup.
    for (const auto* entry : {&outer, &incoming_base(outer)}) {
    const auto& a = *entry;
    if (!a.degree || root.deleted) continue;
    auto add = [&](const neighbor_fragment_t& f) {
      const auto c = static_cast<size_t>(f.page.space());
      for (uint32_t i = 0; i < f.count; ++i) {
        live[c].insert(f.page.slot() + i);
        if (a.degree * 4 + 72 > 4096) giant[c].insert(f.page.slot() + i);
      }
    };
    add(a.first); for (const auto& f : a.rest) add(f);
    }
  }
  for (size_t c = 0; c < 4; ++c) {
    const auto capacity = options_.groups[c].chunk_count * options_.groups[c].chunk_size;
    if (require_all_giants && (giant[c].size() > capacity ||
        (giant[c].size() == capacity &&
         live[c].size() > capacity)))
      throw std::runtime_error("Giant warmup exceeds configured cache; "
                               "increase budget or disable full warmup");
    const auto space = static_cast<page_space_t>(c);
    auto retain = [&](uint32_t p) {
      auto id = page_id_t::make(space, p);
      auto pin = pool_->pin_page(id);
      auto guard = pool_->fetch_read(id, hints_[c][p]);
      validate_base(id, guard.bytes());
      resident_[c][p] = guard.bytes().data();
      resident_pins_.push_back(std::move(pin));
    };
    // Immutable base permits borrowing resident bytes. These pins are actual pool
    // frames, charged to the configured capacity, never a second adjacency cache.
    if (capacity >= live[c].size()) {
      for (auto p : live[c]) {
        if (smo_enabled_) { auto guard = pool_->fetch_read(page_id_t::make(space, p)); }
        else retain(p);
      }
      continue;
    }
    // Reserve room for all giant fragments, then load them last. No extra hidden cache.
    size_t ordinary = capacity > giant[c].size() ? capacity - giant[c].size() : 0;
    for (auto p : live[c]) {
      if (!ordinary) break;
      if (giant[c].contains(p)) continue;
      { auto guard = pool_->fetch_read(page_id_t::make(space, p)); }
      --ordinary;
    }
    for (auto p : giant[c]) {
      if (require_all_giants && !smo_enabled_) retain(p);
      else { auto guard = pool_->fetch_read(page_id_t::make(space, p)); }
    }
  }
}
} // namespace bw_graph
