#include "bw_graph/algo/async/algorithms.h"
#include <bit>

namespace bw_graph::async_algo {
byo::bfs_result_t page_bfs(const byo::graph_t& graph, v_id_t source,
                          options_t options, stats_t* stats) {
  if (!graph.db().multiscale)
    throw std::invalid_argument("Page relaxation requires native storage");
  if (source >= graph.n()) throw std::out_of_range("Page BFS source");
  if (!options.workers || !options.window)
    throw std::invalid_argument("Positive async options required");
  auto& store = *graph.db().multiscale;
  auto order = store.scan_order(); auto groups = store.scan_groups();
  std::array<uint64_t, 4> offsets{};
  size_t pages = 0;
  for (size_t c = 0; c < 4; ++c) {
    offsets[c] = pages;
    pages += store.disk().get_page_count(static_cast<page_space_t>(c));
  }
  if (pages >= UINT32_MAX) throw std::overflow_error("Page BFS directory too large");
  auto key = [&](v_id_t v) {
    auto id = store.location(v).leaf;
    return offsets[static_cast<size_t>(id.space())] + id.slot();
  };
  std::vector<uint32_t> group_id(pages, UINT32_MAX);
  for (size_t i = 0; i + 1 < groups.size(); ++i) group_id[key(order[groups[i]])] = i;
  std::vector<uint64_t> state(options.compact ? 0 : graph.n(), UINT64_MAX);
  std::vector<uint64_t> dirty((pages + 63) / 64, 0);
  byo::bfs_result_t result{std::vector<v_id_t>(options.compact ? graph.n() : 0, UINT32_MAX),
                           std::vector<uint32_t>(graph.n(), UINT32_MAX)};
  // Compact mode uses the output distance's high bit as a short writer lock.
  // A one-bit vertex frontier replaces the extra processed-distance array.
  auto& processed = result.distance; // Only used as processed scratch in packed mode.
  std::vector<uint64_t> active(options.compact ? (graph.n() + 63) / 64 : 0, 0);
  constexpr uint32_t updating = uint32_t(1) << 31;
  if (options.compact && graph.n() >= updating)
    throw std::overflow_error("Compact page BFS requires fewer than 2^31 vertices");
  std::vector<uint32_t> priority(pages, UINT32_MAX);
  if (options.compact) {
    result.distance[source] = 0; result.parent[source] = source;
    active[source / 64] |= uint64_t(1) << (source % 64);
  }
  else state[source] = source;
  if (graph.degree(source)) priority[key(source)] = 0;
  if (graph.degree(source)) dirty[key(source) / 64] |= uint64_t(1) << (key(source) % 64);
  std::vector<uint32_t> frontier; frontier.reserve(pages);
  tbb::enumerable_thread_specific<stats_t> local;
  for (;;) {
    frontier.clear();
    for (size_t i = 0; i < dirty.size(); ++i) {
      auto bits = std::atomic_ref(dirty[i]).exchange(0, std::memory_order_relaxed);
      while (bits) {
        const auto p = i * 64 + std::countr_zero(bits); bits &= bits - 1;
        frontier.push_back(p);
      }
    }
    if (frontier.empty()) break;
    uint32_t lowest = UINT32_MAX;
    for (auto p : frontier) lowest = std::min(lowest, priority[p]);
    const uint64_t bound = uint64_t(lowest) + options.distance_window;
    size_t keep = 0;
    for (auto p : frontier) {
      if (priority[p] <= bound) frontier[keep++] = p;
      else dirty[p / 64] |= uint64_t(1) << (p % 64);
    }
    frontier.resize(keep);
    auto schedule = [&](size_t p, uint32_t distance) {
      auto old = std::atomic_ref(priority[p]).load(std::memory_order_relaxed);
      while (old > distance && !std::atomic_ref(priority[p]).compare_exchange_weak(
          old, distance, std::memory_order_relaxed)) {}
      // Every distance improvement must publish a notification, even when a smaller
      // page priority already exists. A worker can consume that earlier notification
      // while this update is arriving; suppressing the new RMW can lose the wakeup.
      std::atomic_ref(dirty[p / 64]).fetch_or(uint64_t(1) << (p % 64),
                                           std::memory_order_release);
    };
    auto page_vertices = [&](size_t i) {
      const auto g = group_id[frontier[i]];
      if (g == UINT32_MAX) throw std::runtime_error("Active page absent from scan directory");
      return order.subspan(groups[g], groups[g + 1] - groups[g]);
    };
    auto process_page = [&](size_t i) {
      const auto p = frontier[i];
      const auto mask = uint64_t(1) << (p % 64);
      auto vertices = page_vertices(i);
      auto& s = local.local(); ++s.tasks;
      // Retain the active payload page across local waves. This pin uses the same
      // pool frame; releasing it after each wave would let other workers evict it.
      page_pin_t retained;
      if (!graph.fits())
        retained = store.pool().pin_page(store.location(vertices.front()).first.page);
      // No two workers own the same page. A CAS improvement schedules the target page
      // before this wave finishes; exchange-and-collect after the join cannot lose work.
      for (size_t wave = 0; wave < 16; ++wave) {
        std::atomic_ref(dirty[p / 64]).fetch_and(~mask, std::memory_order_acquire);
        std::atomic_ref(priority[p]).store(UINT32_MAX, std::memory_order_relaxed);
        store.visit_base_group(vertices, [&](v_id_t v, auto&& range) {
          if (options.compact && !(std::atomic_ref(active[v / 64]).fetch_and(
              ~(uint64_t(1) << (v % 64)), std::memory_order_acquire) &
              (uint64_t(1) << (v % 64)))) return;
          auto distance = options.compact ?
              std::atomic_ref(result.distance[v]).load(std::memory_order_acquire) :
              uint32_t(std::atomic_ref(state[v]).load(std::memory_order_relaxed) >> 32);
          if (options.compact) distance &= ~updating;
          else if (distance >= processed[v]) return;
          if (distance > bound) {
            if (options.compact) std::atomic_ref(active[v / 64]).fetch_or(
                uint64_t(1) << (v % 64), std::memory_order_release);
            schedule(p, distance); return;
          }
          if (!options.compact) processed[v] = distance;
          ++s.vertices; s.edges += range.size();
          const uint64_t candidate = uint64_t(distance + 1) << 32 | v;
          byo::graph_t::map_range(range, [&](v_id_t u) {
            bool changed = false;
            if (options.compact) {
              auto label = std::atomic_ref(result.distance[u]);
              auto old = label.load(std::memory_order_acquire);
              for (;;) {
                if (old != UINT32_MAX && (old & updating)) {
                  old = label.load(std::memory_order_acquire); continue;
                }
                if (old <= distance + 1) break;
                if (label.compare_exchange_weak(old, updating | (distance + 1),
                                                std::memory_order_acq_rel)) {
                  // Serialize parent writes without a second label array or final I/O scan.
                  result.parent[u] = v;
                  label.store(distance + 1, std::memory_order_release);
                  changed = true; break;
                }
              }
            } else {
              auto old = std::atomic_ref(state[u]).load(std::memory_order_relaxed);
              while ((old >> 32) > (candidate >> 32)) {
                if (std::atomic_ref(state[u]).compare_exchange_weak(old, candidate,
                                                                   std::memory_order_relaxed)) {
                  changed = true; break;
                }
              }
            }
            if (changed && graph.degree(u) > 0) {
              if (options.compact) std::atomic_ref(active[u / 64]).fetch_or(
                  uint64_t(1) << (u % 64), std::memory_order_release);
              schedule(key(u), distance + 1);
            }
          });
        });
        if (!(std::atomic_ref(dirty[p / 64]).load(std::memory_order_relaxed) & mask) ||
            std::atomic_ref(priority[p]).load(std::memory_order_relaxed) > bound) break;
      }
    };
    // I/O pipelining can be selected independently; its pins remain inside the pool.
    if (options.io && !graph.fits()) {
      // Invoke once per page via its first vertex. Other callbacks are deliberately empty.
      for_pages(store, frontier.size(), page_vertices, [](v_id_t) { return true; },
          [&](v_id_t v, auto&&) {
        const auto p = key(v); const auto g = group_id[p];
        if (v != order[groups[g]]) return;
        // frontier is sorted by page key; no auxiliary per-vertex lookup is allocated.
        auto it = std::lower_bound(frontier.begin(), frontier.end(), p);
        process_page(it - frontier.begin());
      }, options, stats);
    } else {
      tbb::parallel_for(tbb::blocked_range<size_t>(0, frontier.size(), 4), [&](const auto& r) {
        for (size_t i = r.begin(); i < r.end(); ++i) process_page(i);
      });
    }
  }
  if (!options.compact) {
    result.parent.resize(graph.n());
    tbb::parallel_for(size_t(0), graph.n(), [&](size_t v) {
      result.parent[v] = state[v]; result.distance[v] = state[v] >> 32;
    });
  }
  if (stats) {
    for (auto& s : local) stats->add(s);
    stats->auxiliary_bytes += (active.capacity() + state.capacity()) * sizeof(uint64_t) +
        priority.capacity() * sizeof(uint32_t) + group_id.capacity() * sizeof(uint32_t) +
        dirty.capacity() * sizeof(uint64_t) + frontier.capacity() * sizeof(uint32_t);
  }
  return result;
}
} // namespace bw_graph::async_algo
