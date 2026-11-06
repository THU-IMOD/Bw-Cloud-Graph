#include "bw_graph/db/multiscale_store.h"
#ifdef BW_GRAPH_ENABLE_TRANSACTION
namespace bw_graph {
uint64_t multiscale_store_t::begin_snapshot() {
  std::lock_guard lock(transaction_mutex_);
  const auto ts = committed_ts_.load(std::memory_order_acquire);
  ++snapshots_[ts];
  return ts;
}
void multiscale_store_t::end_snapshot(uint64_t ts) {
  std::lock_guard lock(transaction_mutex_);
  auto it = snapshots_.find(ts);
  if (it == snapshots_.end()) throw std::logic_error("Unknown native snapshot");
  if (!--it->second) snapshots_.erase(it);
}
uint64_t multiscale_store_t::watermark() {
  std::lock_guard lock(transaction_mutex_);
  return snapshots_.empty() ? committed_ts_.load() : snapshots_.begin()->first;
}
bool multiscale_store_t::commit_edges(uint64_t read_ts, const std::vector<write_opt_t>& writes) {
  bool attaching = false;
  for (const auto& w : writes)
    if (w.src < vertex_count() &&
        (vertices_[w.src].unattached.load() ||
         (w.dst < vertex_count() && vertices_[w.dst].unattached.load()))) attaching = true;
  std::unique_lock exclusive(topology_mutex_, std::defer_lock);
  std::shared_lock shared(topology_mutex_, std::defer_lock);
  if (attaching) exclusive.lock(); else shared.lock();
  std::lock_guard lock(transaction_mutex_);
  // Reject unsupported vertex transactions BEFORE changing any data. Their
  // lifetime needs an MVCC vertex directory, not the non-transaction deleted flag.
  for (const auto& w : writes) {
    if (w.type != write_ops_type_t::INSERT_EDGE && w.type != write_ops_type_t::DELETE_EDGE)
      throw std::invalid_argument(
          "Native snapshot transactions currently support edge operations only");
    if (w.src >= vertex_count() || w.dst >= vertex_count() ||
        vertices_[w.src].deleted || vertices_[w.dst].deleted) return false;
    auto it = edge_commits_.find((uint64_t(w.src) << 32) | w.dst);
    if (it != edge_commits_.end() && it->second > read_ts) return false;
  }
  if (writes.empty()) return true;
  if (attaching) {
    orphan_page_ = {};
    std::map<v_id_t, std::vector<v_id_t>> neighbors;
    for (const auto& w : writes) if (w.type == write_ops_type_t::INSERT_EDGE)
      neighbors[w.src].push_back(w.dst);
    for (const auto& [v, adjacent] : neighbors) attach_locked(v, adjacent);
  }
  const uint64_t timestamp = committed_ts_.load() + 1;
  std::map<v_id_t, std::pair<uint64_t, uint64_t>> old_heads;
  // Allocate conflict metadata before publishing records; allocation failure
  // cannot turn an unsuccessful commit into partially visible writes.
  for (const auto& w : writes) {
    for (auto v : {w.src, w.dst})
      old_heads.try_emplace(v, vertices_[v].head.load(), vertices_[v].in_head.load());
    edge_commits_.try_emplace((uint64_t(w.src) << 32) | w.dst, 0);
  }
  try {
    for (const auto& w : writes)
      if (!update_pair(w.src, w.dst, w.type == write_ops_type_t::INSERT_EDGE ? INSERT : DELETE,
                         timestamp, read_ts == UINT64_MAX ? timestamp - 1 : read_ts))
        throw std::runtime_error("Native transaction append failed");
  } catch (...) {
    for (auto [v, head] : old_heads) {
      std::lock_guard vertex_lock(writer(v).mutex);
      vertices_[v].head.store(head.first, std::memory_order_release);
      vertices_[v].in_head.store(head.second, std::memory_order_release);
    }
    throw;
  }
  for (const auto& w : writes) edge_commits_[(uint64_t(w.src) << 32) | w.dst] = timestamp;
  // A new snapshot cannot start until every record is published. Existing
  // snapshots exclude this timestamp, including during multi-vertex publication.
  committed_ts_.store(timestamp, std::memory_order_release);
  index_dirty_.store(true, std::memory_order_release);
  return true;
}
} // namespace bw_graph
#endif
