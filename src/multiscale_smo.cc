#include "bw_graph/db/multiscale_store.h"
#include "bw_graph/io/directional_csr_page.h"
#include <algorithm>
#include "bw_graph/part/reorder.h"
#include <set>
#include <map>
#include <unordered_map>

namespace bw_graph {
namespace {
void write_csr_image(multiscale_buf_pool_t& pool, page_id_t id, size_t size,
                     std::span<const directional_csr_page_t::input_t> vertices, uint64_t ts) {
  auto image = directional_csr_page_t::build(size, id.raw(), vertices, ts);
  auto guard = pool.new_page(id);
  std::memcpy(guard.bytes().data(), image.data(), image.size());
}
} // namespace

page_id_t multiscale_store_t::allocate_runtime_page(page_space_t space) {
  auto& reservation = reservations_[static_cast<size_t>(space)];
  std::lock_guard lock(reservation.mutex);
  if (!reservation.remaining) {
    // Amortize file-length updates, with <=256 KiB reserved per space. No page
    // reuse/ABA: unconsumed slots remain zero and are skipped after reopening.
    const uint32_t count = std::max<uint32_t>(1, 262144 / disk_.page_size(space));
    auto first = disk_.allocate_pages(space, count);
    reservation.next = first.slot(); reservation.remaining = count;
  }
  --reservation.remaining;
  return page_id_t::make(space, reservation.next++);
}
void multiscale_store_t::initialize_writers() {
  writers_.grow_to_at_least(disk_.get_page_count(page_space_t::CSR_4K));
  for (v_id_t v = 0; v < vertices_.size(); ++v) {
    auto id = vertices_[v].anchor;
    if (!id.valid() || id.space() != page_space_t::CSR_4K || id.slot() >= writers_.size())
      throw std::runtime_error("Invalid vertex anchor");
    auto& w = writers_[id.slot()];
    if (!w) w = std::make_unique<delta_writer_t>();
    w->members.push_back(v);
    for (auto head : {vertices_[v].head.load(), vertices_[v].in_head.load()})
    if (head != UINT64_MAX) {
      auto page = page_id_t::from_raw(head >> 32);
      if (!w->chain.valid() || page.raw() > w->chain.raw()) w->chain = page;
    }
  }
  // Resume the final ID-ordered reception page after a normal reopen.
  for (const auto& leaf : tat_.leaves()) {
    if (leaf.vertices.size() >= (4096 - 40) / 32 || leaf.vertices.empty()) continue;
    if (leaf.vertices.back() + 1 != vertices_.size()) continue;
    bool orphan = true;
    for (auto v : leaf.vertices)
      orphan &= vertices_[v].unattached && vertices_[v].head == UINT64_MAX && vertices_[v].in_head == UINT64_MAX;
    if (orphan) orphan_page_ = leaf.page;
  }
}
void multiscale_store_t::queue_smo(uint32_t anchor, delta_writer_t& w) {
  // Caller holds the anchor's append mutex; queue mutex is never held during I/O.
  if (w.queued) return;
  std::lock_guard lock(queue_mutex_);
  if (worker_error_) std::rethrow_exception(worker_error_);
  jobs_.push_back(anchor);
  w.queued = true;
  queue_cv_.notify_one();
}
void multiscale_store_t::worker_loop() {
  for (;;) {
    std::vector<uint32_t> anchors;
    {
      std::unique_lock lock(queue_mutex_);
      queue_cv_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty()) return;
      // Bound each worker's batch. Packing across ready anchors avoids a nearly
      // empty output page per anchor, without delaying work for a timer.
      while (!jobs_.empty() && anchors.size() < 32) {
        anchors.push_back(jobs_.front()); jobs_.pop_front();
      }
      ++active_jobs_;
    }
    bool succeeded = true;
    try {
      consolidate(anchors, false);
    } catch (...) {
      succeeded = false;
      std::lock_guard lock(queue_mutex_);
      if (!worker_error_) worker_error_ = std::current_exception();
    }
    for (auto anchor : anchors) {
      auto& w = *writers_[anchor];
      std::lock_guard lock(w.mutex);
      w.queued = false;
      // Only new writes count toward rescheduling; deferred frozen records do not.
#ifdef BW_GRAPH_ENABLE_TRANSACTION
      constexpr size_t record_bytes = 40;
#else
      constexpr size_t record_bytes = 24;
#endif
      if (succeeded && w.pending >= (delta_threshold_ - 1) * ((4096 - 32) / record_bytes) + 1) {
        try { queue_smo(anchor, w); }
        catch (...) {
          std::lock_guard queue_lock(queue_mutex_);
          if (!worker_error_) worker_error_ = std::current_exception();
        }
      }
    }
    {
      std::lock_guard lock(queue_mutex_);
      --active_jobs_;
    }
    queue_cv_.notify_all();
  }
}
void multiscale_store_t::wait_for_smo() {
  std::unique_lock lock(queue_mutex_);
  queue_cv_.wait(lock, [&] { return jobs_.empty() && !active_jobs_; });
  if (worker_error_) std::rethrow_exception(worker_error_);
}
multiscale_store_t::smo_stats_t multiscale_store_t::smo_stats() const {
  return {smo_jobs_, smo_vertices_, smo_records_, smo_deferred_, smo_bytes_, smo_background_};
}
void multiscale_store_t::consolidate_all() {
  // Producers are quiescent at this explicit drain boundary. Order the pending
  // anchors by original CSR page ID to improve base-read locality with a small cache;
  // online submissions retain FIFO scheduling and cannot starve behind hot IDs.
  {
    std::lock_guard lock(queue_mutex_);
    std::sort(jobs_.begin(), jobs_.end());
  }
  wait_for_smo();
  if (!smo_enabled_) return;
  std::vector<uint32_t> anchors;
  for (uint32_t a = 0; a < writers_.size(); ++a) if (writers_[a]) {
    anchors.push_back(a);
    if (anchors.size() == 32) { consolidate(anchors, true); anchors.clear(); }
  }
  if (!anchors.empty()) consolidate(anchors, true);
  refresh_scan_order();
}
void multiscale_store_t::write_base(v_id_t v, std::span<const v_id_t> edges,
                                    adjacency_t& out, unsigned max_scale, direction_t direction) {
  using input_t = directional_csr_page_t::input_t;
  size_t offset = 0;
  for (int c = std::min(3u, max_scale); c >= 0 && offset < edges.size(); --c) {
    if (!options_.groups[c].chunk_count || !options_.groups[c].chunk_size) continue;
    const auto space = static_cast<page_space_t>(c);
    const size_t capacity = (disk_.page_size(space) - 72) / 4;
    size_t pages = (edges.size() - offset) / capacity;
    if (!c && (edges.size() - offset) % capacity) ++pages;
    if (!pages) continue;
    auto first = pages == 1 ? allocate_runtime_page(space) : disk_.allocate_pages(space, pages);
    const size_t count = std::min(edges.size() - offset, pages * capacity);
    neighbor_fragment_t extent{first, 72, uint32_t(pages), count};
    if (!out.first.count) out.first = extent; else out.rest.push_back(extent);
    out.degree += count;
    for (size_t p = 0; p < pages; ++p) {
      auto id = page_id_t::make(space, first.slot() + p);
      auto n = std::min(capacity, edges.size() - offset);
      input_t input{v, {}, 0, {}};
      (direction == direction_t::out ? input.neighbors : input.incoming) = edges.subspan(offset, n);
      write_csr_image(*pool_, id, disk_.page_size(space), {&input, 1}, out.base_ts);
      smo_bytes_.fetch_add(disk_.page_size(space), std::memory_order_relaxed);
      offset += n;
    }
  }
  if (offset != edges.size()) throw std::runtime_error("CSR 4K frames required for SMO tails");
}
void multiscale_store_t::consolidate(std::span<const uint32_t> anchors, bool force) {
  std::shared_lock topology(topology_mutex_);
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  const uint64_t safe_ts = watermark();
#else
  const uint64_t safe_ts = 0;
#endif
  struct pending_t {
    v_id_t v;
    std::shared_ptr<adjacency_t> base;
    uint64_t head;
    std::shared_ptr<adjacency_t> next;
    std::vector<v_id_t> small;
    page_id_t old_leaf;
    bool changed{false}, inline_edges{false};
    std::shared_ptr<pending_t> incoming{};
  };
  std::vector<pending_t> work;
  for (auto anchor : anchors) {
    auto& writer = *writers_[anchor];
    std::lock_guard lock(writer.mutex);
    writer.pending = 0;
    std::vector<pending_t> captured;
    captured.reserve(writer.members.size());
    std::set<uint32_t> dirty;
    for (auto v : writer.members) {
      auto& a = vertices_[v];
      auto base = std::atomic_load(&a.version);
      const auto& current = base ? *base : a;
      auto head = a.head.load(std::memory_order_acquire);
      auto in_head = a.in_head.load(std::memory_order_acquire);
      if (!a.deleted && ((head != current.stop && head != UINT64_MAX) ||
                        (in_head != incoming_base(current).stop && in_head != UINT64_MAX)))
        dirty.insert(current.leaf.raw());
      auto in = std::make_shared<pending_t>();
      in->v = v; in->base = current.incoming; in->head = in_head; in->old_leaf = current.leaf;
      captured.push_back({v, std::move(base), head, {}, {}, current.leaf});
      captured.back().incoming = std::move(in);
    }
    if (dirty.empty()) continue;
    std::map<uint32_t, std::vector<pending_t>> leaves;
    for (auto& item : captured) if (dirty.contains(item.old_leaf.raw()))
      leaves[item.old_leaf.raw()].push_back(std::move(item));
    for (auto leaf : dirty) {
      auto& items = leaves.at(leaf);
      // A leaf never spans stable Delta anchors. Preserve physical slot order
      // on no-split rewrites; Gorder is used below when the leaf overflows.
      std::unordered_map<v_id_t, size_t> positions;
      for (size_t i = 0; i < items.size(); ++i) positions[items[i].v] = i;
      for (auto v : tat_.members(page_id_t::from_raw(leaf)))
        work.push_back(std::move(items.at(positions.at(v))));
    }
  }
  if (work.empty()) return;
  smo_jobs_ += anchors.size();
  if (!force) smo_background_ += anchors.size();
  // Bounded SMO scratch: decode each shared Delta page once instead of pinning
  // it for every vertex record. At most 32 KiB per worker, never a neighbor cache.
  std::unordered_map<uint32_t, std::array<std::byte, 4096>> delta_images;
  auto cached_delta = [&](uint64_t pointer) {
    const uint32_t raw = pointer >> 32;
    auto it = delta_images.find(raw);
    if (it == delta_images.end()) {
      if (delta_images.size() == 8) delta_images.clear();
      auto guard = pool_->fetch_read(page_id_t::from_raw(raw));
      it = delta_images.try_emplace(raw).first;
      std::memcpy(it->second.data(), guard.bytes().data(), 4096);
    }
    return decode_delta(pointer, it->second);
  };
  std::optional<read_page_guard_t> shared_base;
  page_id_t shared_id;
  std::vector<std::pair<pending_t*, direction_t>> directions;
  for (auto& item : work) {
    directions.emplace_back(&item, direction_t::out);
    directions.emplace_back(item.incoming.get(), direction_t::in);
  }
  for (auto [pointer, direction] : directions) {
    auto& item = *pointer;
    auto& root = vertices_[item.v];
    const auto& base = item.base ? *item.base :
        (direction == direction_t::out ? root : incoming_base(root));
#ifdef BW_GRAPH_ENABLE_TRANSACTION
    while (item.head != UINT64_MAX && item.head != base.stop) {
      auto record = cached_delta(item.head);
      if (uint64_t(record.commit_ts) <= safe_ts) break;
      item.head = (uint64_t(record.next_page_no) << 32) | record.next_record_idx;
    }

#endif
    struct change_t { delta_type_t operation; uint64_t writes; };
    std::map<v_id_t, change_t> changes;
    uint64_t records = 0;
    for (auto p = item.head; p != base.stop && p != UINT64_MAX;) {
      auto rec = cached_delta(p);
      if (rec.first != item.v || rec.direction != direction)
        throw std::runtime_error("SMO Delta direction mismatch");
      auto [change, inserted] = changes.try_emplace(rec.second, change_t{rec.type, 0});
      (void)inserted;
      ++change->second.writes;
      ++records;
      p = (uint64_t(rec.next_page_no) << 32) | rec.next_record_idx;
    }
    const auto reads = records ? root.reads.exchange(0, std::memory_order_relaxed) : 0;
    // Initial explicit cost model: byte-equivalent sequential rewrite cost versus
    // repeated delta traversal. Constants are conservative starting estimates,
    // not a claimed hardware optimum. Force always materializes the frozen prefix.
    const bool giant = base.degree * 4 + 72 > 4096;
    uint64_t page_count = base.first.count;
    for (const auto& extent : base.rest) page_count += extent.count;
    const uint64_t rewrite_cost = base.degree * 4;
#ifdef BW_GRAPH_ENABLE_TRANSACTION
    constexpr uint64_t record_bytes = 40;
#else
    constexpr uint64_t record_bytes = 24;
#endif
    const uint64_t delta_cost = records * record_bytes * (1 + reads) +
        (page_count > 8 ? reads * (page_count - 1) * 512 : 0);
    if (giant && !force && records < 4096 && delta_cost < rewrite_cost) {
      smo_deferred_ += records; changes.clear(); records = 0;
    }
    item.next = std::make_shared<adjacency_t>();
    auto& out = *item.next;
    out.anchor = root.anchor; out.leaf = base.leaf;
    out.stop = records ? item.head : base.stop;
    out.base_ts = records ? safe_ts : base.base_ts;
    item.changed = records != 0;
    if (!records && giant) {
      out.degree = base.degree; out.first = base.first; out.rest = base.rest;
      continue;
    }
    // Compact one ordered extent at a time. Unchanged extents are retained by ID.
    // The current format has no range synopsis: discovering boundaries still
    // scans the base, but only affected ranges are rewritten.
    std::vector<v_id_t> old;
    old.reserve(base.degree);
    if (base.degree && base.first.count == 1 && base.rest.empty()) {
      if (shared_id != base.first.page) {
        shared_base.reset();
        shared_base.emplace(pool_->fetch_read(base.first.page));
        shared_id = base.first.page;
        validate_base(shared_id, shared_base->bytes());
      }
      old.resize(base.degree);
      std::memcpy(old.data(), shared_base->bytes().data() + base.first.byte_offset,
                  old.size() * sizeof(v_id_t));
    } else {
      shared_base.reset(); shared_id = {};
      neighbor_range_t range(*this, base);
      for (auto dst : range) old.push_back(dst);
    }
    auto apply = [&](std::span<const v_id_t> input, auto begin, auto end) {
      std::vector<v_id_t> result;
      result.reserve(input.size() + std::distance(begin, end));
      size_t i = 0;
      for (auto it = begin; it != end; ++it) {
        while (i < input.size() && input[i] < it->first) result.push_back(input[i++]);
        if (i < input.size() && input[i] == it->first) ++i;
        if (it->second.operation == INSERT) result.push_back(it->first);
      }
      result.insert(result.end(), input.begin() + i, input.end());
      return result;
    };
    if (!giant || old.size() <= changes.size() + (4096 - 72) / 4) {
      item.small = apply(old, changes.begin(), changes.end());
      item.inline_edges = item.small.size() * 4 + 72 <= 4096;
      if (!item.inline_edges) {
        write_base(item.v, item.small, out, 3, direction); item.small.clear();
      }
    } else if (page_count > 8 && reads > records * 4) {
      // Hysteresis: only repeated reads justify coalescing a fragmented cold base.
      auto merged = apply(old, changes.begin(), changes.end());
      write_base(item.v, merged, out, 3, direction);
    } else {
      size_t offset = 0;
      auto change = changes.begin();
      std::vector<neighbor_fragment_t> extents;
      auto expand = [&](const neighbor_fragment_t& run) {
        uint64_t left = run.edges;
        const uint64_t capacity = (disk_.page_size(run.page.space()) - run.byte_offset) / 4;
        for (uint32_t p = 0; p < run.count; ++p) {
          auto n = std::min(left, capacity);
          extents.push_back({page_id_t::make(run.page.space(), run.page.slot() + p),
                             run.byte_offset, 1, n});
          left -= n;
        }
      };
      expand(base.first);
      for (const auto& run : base.rest) expand(run);
      for (size_t e = 0; e < extents.size(); ++e) {
        const auto& extent = extents[e];
        const size_t n = extent.edges;
        auto end = e + 1 == extents.size() ? changes.end() :
            changes.upper_bound(old.at(offset + n - 1));
        if (change == end) {
          if (!out.first.count) out.first = extent; else out.rest.push_back(extent);
          out.degree += n;
        } else {
          auto merged = apply(std::span<const v_id_t>(old).subspan(offset, n), change, end);
          // Hot ranges are split into smaller rewrite units. Read-dominant ranges
          // can again use large contiguous pages on subsequent consolidation.
          // Repeated writes to one neighbor are hot too. Counting only distinct
          // neighbor IDs would incorrectly keep this range in a large page.
          uint64_t updates = 0;
          for (auto it = change; it != end; ++it) updates += it->second.writes;
          const auto current_scale = static_cast<unsigned>(extent.page.space());
          unsigned scale = updates > std::max<uint64_t>(8, reads * 4) ?
              (current_scale ? current_scale - 1 : 0) : 3;
          write_base(item.v, merged, out, scale, direction);
        }
        offset += n; change = end;
      }
    }
    smo_records_ += records;
    if (records) ++smo_vertices_;
  }
  shared_base.reset();
  for (auto& item : work) {
    auto& in = *item.incoming;
    if (72 + item.small.size() * 4 + in.small.size() * 4 > 4096) {
      auto* spill = item.small.size() >= in.small.size() ? &item : &in;
      auto direction = spill == &item ? direction_t::out : direction_t::in;
      write_base(item.v, spill->small, *spill->next, 3, direction);
      spill->small.clear(); spill->inline_edges = false;
    }
    item.next->incoming = in.next;
  }
  // Rewrite a complete current leaf, including unchanged vertex slots and
  // giant metadata. Never mix independent leaves/anchors in an output page.
  using input_t = directional_csr_page_t::input_t;
  size_t begin = 0;
  while (begin < work.size()) {
    size_t end = begin + 1;
    while (end < work.size() && work[end].old_leaf == work[begin].old_leaf) ++end;
    bool changed = false;
    size_t required = 40;
    std::vector<pending_t*> order;
    for (size_t i = begin; i < end; ++i) {
      changed |= work[i].changed || work[i].incoming->changed;
      required += 32 + (work[i].small.size() + work[i].incoming->small.size()) * 4;
      order.push_back(&work[i]);
    }
    if (!changed) { begin = end; continue; }
    if (required > 4096) {
      adj_map_t graph;
      std::unordered_map<v_id_t, pending_t*> by_vertex;
      for (auto* item : order) {
        by_vertex.emplace(item->v, item);
        if (item->inline_edges) graph.emplace(item->v, item->small);
        else {
          std::vector<v_id_t> edges;
          neighbor_range_t range(*this, *item->next);
          for (auto dst : range) edges.push_back(dst);
          graph.emplace(item->v, std::move(edges));
        }
      }
      order.clear();
      for (auto v : gorder_reordering_optimized(graph)) order.push_back(by_vertex.at(v));
      if (order.size() != end - begin) throw std::logic_error("Incomplete CSR Gorder");
    }
    std::vector<input_t> inputs;
    std::vector<pending_t*> packed;
    std::vector<multiscale_tat_t::leaf_t> leaves;
    size_t used = 40;
    auto flush = [&] {
      if (inputs.empty()) return;
      auto id = allocate_runtime_page(page_space_t::CSR_4K);
      write_csr_image(*pool_, id, 4096, inputs, safe_ts);
      smo_bytes_ += 4096;
      multiscale_tat_t::leaf_t leaf{id, {}};
      uint32_t offset = 40 + inputs.size() * 32;
      for (size_t i = 0; i < packed.size(); ++i) {
        auto& out = *packed[i]->next;
        out.leaf = id;
        leaf.vertices.push_back(packed[i]->v);
        auto& in = *packed[i]->incoming->next;
        in.leaf = id;
        if (packed[i]->inline_edges) {
          out.degree = inputs[i].neighbors.size();
          out.first = out.degree ? neighbor_fragment_t{id, offset, 1, out.degree} : neighbor_fragment_t{};
        }
        offset += inputs[i].neighbors.size_bytes();
        if (packed[i]->incoming->inline_edges) {
          in.degree = inputs[i].incoming.size();
          in.first = in.degree ? neighbor_fragment_t{id, offset, 1, in.degree} : neighbor_fragment_t{};
        }
        offset += inputs[i].incoming.size_bytes();
      }
      leaves.push_back(std::move(leaf));
      inputs.clear(); packed.clear(); used = 40;
    };
    for (auto* item : order) {
      const size_t needed = 32 + (item->small.size() + item->incoming->small.size()) * 4;
      if (used + needed > 4096) flush();
      inputs.push_back({item->v, item->small, 0, item->incoming->small}); packed.push_back(item); used += needed;
    }
    flush();
    // The stable anchor mutex serializes publication with append. Readers keep
    // immutable generations; their old leaf and extent pages remain valid.
    const auto anchor = vertices_[work[begin].v].anchor;
    {
      std::lock_guard lock(writers_[anchor.slot()]->mutex);
      for (size_t i = begin; i < end; ++i)
        std::atomic_store_explicit(&vertices_[work[i].v].version, work[i].next,
                                  std::memory_order_release);
    }
    // Internal overflow splits propagate to the root using a quotient graph of
    // the now-published base. Index changes are resident until checkpoint.
    tat_.replace(work[begin].old_leaf, std::move(leaves),
                 [this](auto v) { return topology_edges(v); });
    begin = end;
  }
  index_dirty_.store(true, std::memory_order_release);
}
} // namespace bw_graph
