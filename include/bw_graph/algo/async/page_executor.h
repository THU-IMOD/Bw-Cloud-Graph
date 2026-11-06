#pragma once
#include "bw_graph/db/multiscale_store.h"
#include <tbb/parallel_for.h>
#include <chrono>

namespace bw_graph::async_algo {
struct stats_t {
  uint64_t tasks{0}, vertices{0}, edges{0}, async_reads{0}, cache_hits{0};
  uint64_t auxiliary_bytes{0}, wait_ns{0};
  void add(const stats_t& x) {
    tasks += x.tasks; vertices += x.vertices; edges += x.edges;
    async_reads += x.async_reads; cache_hits += x.cache_hits; wait_ns += x.wait_ns;
    auxiliary_bytes = std::max(auxiliary_bytes, x.auxiliary_bytes);
  }
};
struct options_t {
  size_t workers{1};
  size_t window{4}; // Maximum requests per compute worker, backed by pool frames.
  bool io{true};
  uint32_t distance_window{8}; // Page BFS: bound speculative distance propagation.
  // Page BFS: output arrays + one activation bit/vertex, no extra label array.
  bool compact{true};
};

// ACGraph-style readiness scheduling, implemented against the existing six-space pool.
// Requests own pins in that pool; there is no staging adjacency buffer. Frontier/iteration
// semantics are supplied by the caller. This executor does not relax algorithm barriers.
template <class Group, class Eligible, class Visit>
void for_pages(multiscale_store_t& store, size_t count, Group&& group,
               Eligible&& eligible, Visit&& visit, options_t options, stats_t* stats) {
  if (!options.workers || !options.window)
    throw std::invalid_argument("Positive async options required");
  if (!count) return;
  const auto pool_state = store.pool().stats(page_space_t::CSR_4K);
  const size_t free = pool_state.frame_count - pool_state.pinned_frames;
  const auto window = options.io && free > 2 ?
      std::min(options.window, (free - 2) / options.workers) : size_t(0);
  if (!window || store.base_fits()) {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, count, 16), [&](const auto& r) {
      for (size_t i = r.begin(); i < r.end(); ++i) {
        auto vertices = group(i);
        if (std::any_of(vertices.begin(), vertices.end(), eligible))
          store.visit_base_group(vertices, visit);
      }
    });
    return;
  }
  struct task_t {
    std::span<const v_id_t> vertices;
    page_pin_t pin;
    std::future<page_pin_t> future;
  };
  struct pending_t {
    std::vector<task_t> tasks;
    ~pending_t() {
      // Drain accepted requests even if a user callback or an I/O completion throws.
      for (auto& t : tasks) if (t.future.valid()) {
        try { t.pin = t.future.get(); } catch (...) {}
      }
    }
  };
  std::atomic<size_t> next{0};
  std::vector<stats_t> local(options.workers);
  tbb::parallel_for(size_t(0), options.workers, [&](size_t worker) {
    pending_t pending;
    auto& tasks = pending.tasks;
    tasks.reserve(window);
    auto& s = local[worker];
    for (;;) {
      while (tasks.size() < window) {
        const auto i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= count) break;
        auto vertices = group(i);
        if (!std::any_of(vertices.begin(), vertices.end(), eligible)) continue;
        const auto id = store.location(vertices.front()).first.page;
        task_t task{vertices, {}, {}};
        if (id.valid() && id.space() == page_space_t::CSR_4K) {
          task.pin = store.pool().try_pin_page(id);
          if (task.pin) ++s.cache_hits;
          else { task.future = store.pool().fetch_async(id).result; ++s.async_reads; }
        }
        tasks.push_back(std::move(task));
      }
      if (tasks.empty()) break;
      bool progressed = false;
      for (size_t i = 0; i < tasks.size();) {
        auto& t = tasks[i];
        if (t.future.valid() && t.future.wait_for(std::chrono::seconds(0)) !=
                                  std::future_status::ready) { ++i; continue; }
        if (t.future.valid()) t.pin = t.future.get();
        store.visit_base_group(t.vertices, visit);
        ++s.tasks;
        if (i + 1 != tasks.size()) t = std::move(tasks.back());
        tasks.pop_back();
        progressed = true;
      }
      if (!progressed) {
        const auto start = std::chrono::steady_clock::now();
        tasks.front().future.wait();
        s.wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
      }
    }
  });
  if (stats) {
    for (auto& s : local) stats->add(s);
    stats->auxiliary_bytes = std::max<uint64_t>(stats->auxiliary_bytes,
        options.workers * (window * sizeof(task_t) + sizeof(stats_t)));
  }
}
} // namespace bw_graph::async_algo
