#include "bw_graph/db/multiscale_store.h"
#include "bw_graph/io/directional_csr_page.h"
#include <algorithm>

namespace bw_graph {
namespace {
constexpr size_t orphan_capacity = (4096 - 40) / 32;
using input_t = directional_csr_page_t::input_t;
}

v_id_t multiscale_store_t::append_vertex_locked(bool deleted) {
  const auto v = vertex_count();
  if (v == INVALID_VID) throw std::overflow_error("Vertex ID exhausted");
  std::vector<v_id_t> members;
  if (orphan_page_.valid()) members = tat_.members(orphan_page_);
  if (members.size() == orphan_capacity) { orphan_page_ = {}; members.clear(); }
  const bool fresh = !orphan_page_.valid();
  auto page = fresh ? allocate_runtime_page(page_space_t::CSR_4K) : orphan_page_;
  members.push_back(v);
  std::vector<input_t> inputs;
  for (auto id : members) inputs.push_back({id, {}, 0});
  auto bytes = directional_csr_page_t::build(4096, page.raw(), inputs);
  {
    auto guard = fresh ? pool_->new_page(page) : pool_->fetch_write(page);
    std::memcpy(guard.bytes().data(), bytes.data(), bytes.size());
  }
  vertices_.grow_to_at_least(size_t(v) + 1);
  auto& root = vertices_[v];
  root.anchor = page; root.leaf = page; root.unattached = true; root.deleted = deleted;
  auto version = std::make_shared<adjacency_t>();
  version->anchor = page; version->leaf = page;
  std::atomic_store(&root.version, version);
  writers_.grow_to_at_least(size_t(page.slot()) + 1);
  if (!writers_[page.slot()]) writers_[page.slot()] = std::make_unique<delta_writer_t>();
  writers_[page.slot()]->members.push_back(v);
  if (fresh) tat_.append({page, members}, [this](auto u) { return topology_edges(u); });
  else tat_.set_members(page, members);
  orphan_page_ = page;
  published_vertices_.store(v + 1, std::memory_order_release);
  index_dirty_ = true;
  return v;
}

v_id_t multiscale_store_t::append_vertex() { return append_vertex({}); }
v_id_t multiscale_store_t::append_vertex(std::span<const v_id_t> neighbors) {
  std::unique_lock topology(topology_mutex_);
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  std::lock_guard transaction(transaction_mutex_);
  if (!snapshots_.empty())
    throw std::logic_error("Vertex lifetime changes require quiescent native snapshots");
#endif
  std::vector<v_id_t> unique(neighbors.begin(), neighbors.end());
  std::sort(unique.begin(), unique.end());
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  neighbors = unique;
  for (auto dst : neighbors)
    if (dst >= vertex_count() || vertices_[dst].deleted)
      throw std::invalid_argument("Every neighbor must exist before vertex insertion");
  auto v = append_vertex_locked();
  if (!neighbors.empty()) {
    attach_locked(v, neighbors);
    const auto ts = committed_ts_.load() + 1;
    for (auto dst : neighbors) {
      update_pair(v, dst, INSERT, ts, ts - 1);
#ifdef BW_GRAPH_ENABLE_TRANSACTION
      edge_commits_[(uint64_t(v) << 32) | dst] = ts;
#endif
    }
    committed_ts_.store(ts, std::memory_order_release);
  }
  return v;
}

bool multiscale_store_t::insert_vertex(v_id_t v) {
  std::unique_lock topology(topology_mutex_);
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  std::lock_guard transaction(transaction_mutex_);
  if (!snapshots_.empty())
    throw std::logic_error("Vertex lifetime changes require quiescent native snapshots");
#endif
  if (v == INVALID_VID) return false;
  if (v < vertex_count()) {
    const auto restored = vertices_[v].deleted.exchange(false);
    if (restored) index_dirty_ = true;
    return restored;
  }
  while (vertex_count() <= v) append_vertex_locked(vertex_count() != v);
  return true;
}

// Rewrite slot metadata only. Existing immutable base edge fragments remain
// referenced by the new generations, until the normal SMO repacks their edges.
void multiscale_store_t::rewrite_members_locked(std::vector<page_id_t> old,
    std::vector<std::vector<v_id_t>> groups) {
  struct publication_t { v_id_t v; std::shared_ptr<adjacency_t> version; };
  std::vector<publication_t> publications;
  std::vector<multiscale_tat_t::leaf_t> leaves;
  for (auto& group : groups) {
    for (size_t begin = 0; begin < group.size(); begin += orphan_capacity) {
      const auto end = std::min(group.size(), begin + orphan_capacity);
      std::vector<v_id_t> members(group.begin() + begin, group.begin() + end);
      auto page = allocate_runtime_page(page_space_t::CSR_4K);
      std::vector<input_t> inputs;
      for (auto v : members) inputs.push_back({v, {}, 0});
      auto bytes = directional_csr_page_t::build(4096, page.raw(), inputs);
      { auto guard = pool_->new_page(page);
        std::memcpy(guard.bytes().data(), bytes.data(), bytes.size()); }
      writers_.grow_to_at_least(size_t(page.slot()) + 1);
      auto w = std::make_unique<delta_writer_t>();
      w->members = members;
      for (auto v : members) {
        const auto& root = vertices_[v];
        auto current = std::atomic_load(&root.version);
        const auto& base = current ? *current : root;
        auto next = std::make_shared<adjacency_t>();
        next->incoming = base.incoming;
        next->first = base.first; next->rest = base.rest; next->degree = base.degree;
        next->stop = base.stop; next->base_ts = base.base_ts;
        next->leaf = page; next->anchor = page;
        const auto head = std::max(root.head.load() == UINT64_MAX ? 0 : root.head.load(),
                                   root.in_head.load() == UINT64_MAX ? 0 : root.in_head.load());
        if (head != 0 && head != base.stop) {
          auto delta = page_id_t::from_raw(head >> 32);
          if (!w->chain.valid() || delta.raw() > w->chain.raw()) w->chain = delta;
          ++w->pending;
        }
        publications.push_back({v, std::move(next)});
      }
      writers_[page.slot()] = std::move(w);
      leaves.push_back({page, members});
    }
  }
  // Remove migrated vertices from every former owner; an anchor may own several leaves.
  for (auto& publication : publications) {
    auto& root = vertices_[publication.v];
    std::erase(writers_[root.anchor.slot()]->members, publication.v);
    root.anchor = publication.version->anchor;
    std::atomic_store_explicit(&root.version, publication.version, std::memory_order_release);
  }
  tat_.rewrite(old, std::move(leaves), [this](auto v) { return topology_edges(v); });
  if (std::find(old.begin(), old.end(), orphan_page_) != old.end()) orphan_page_ = {};
  index_dirty_ = true;
}

void multiscale_store_t::attach_locked(v_id_t v, std::span<const v_id_t> neighbors) {
  if (neighbors.empty() || !vertices_[v].unattached) return;
  std::map<uint32_t, size_t> votes;
  for (auto dst : neighbors) {
    if (dst == v) continue;
    auto version = std::atomic_load(&vertices_[dst].version);
    ++votes[(version ? version->leaf : vertices_[dst].leaf).raw()];
  }
  if (votes.empty()) { vertices_[v].unattached = false; orphan_page_ = {}; return; }
  auto best = votes.begin();
  for (auto it = votes.begin(); it != votes.end(); ++it)
    if (it->second > best->second) best = it;
  auto target = page_id_t::from_raw(best->first);
  auto current = std::atomic_load(&vertices_[v].version);
  auto source = current ? current->leaf : vertices_[v].leaf;
  if (source != target) {
    auto source_members = tat_.members(source), target_members = tat_.members(target);
    std::erase(source_members, v);
    // Put the attached vertex beside the voted neighbors even if metadata splits.
    target_members.insert(target_members.begin(), v);
    rewrite_members_locked({source, target}, {source_members, target_members});
  }
  vertices_[v].unattached = false;
  // An orphan reception page is sealed as soon as it receives graph updates.
  if (source == orphan_page_ || target == orphan_page_) orphan_page_ = {};
  index_dirty_ = true;
}
} // namespace bw_graph
