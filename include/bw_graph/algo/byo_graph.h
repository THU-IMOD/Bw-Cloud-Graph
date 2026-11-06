#pragma once
// Bw-Graph adaptation of BYO/GBBS. See BYO_LICENSE.txt and README for provenance.
#include "bw_graph/db/db.h"
#include "bw_graph/algo/async/page_executor.h"
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>
#include <tbb/task_arena.h>

namespace bw_graph::byo {
// Algorithms read a quiescent base graph, just like the original map/scan path.
// This adapter owns degree metadata, never an adjacency copy. Keep db alive.
class graph_t {
public:
  explicit graph_t(bw_graph_db_t& db, bool page_aware = true, bool undirected = false);
  bool projected() const { return undirected_; }
  size_t n() const { return degrees_.size(); }
  uint64_t m() const { return edges_; }
  uint64_t degree(v_id_t v) const { return degrees_[v]; }
  size_t metadata_bytes() const { return degrees_.capacity() * sizeof(uint32_t) + isolates_.capacity() * sizeof(v_id_t); }
  bool fits() const { return base_fits_; }
  bool optimized() const { return page_aware_; }
  void set_async(async_algo::options_t options, async_algo::stats_t* stats = nullptr) {
    if (!options.workers || !options.window) throw std::invalid_argument("Positive async options required");
    async_ = options; async_stats_ = stats;
  }
  bw_graph_db_t& db() const { return db_; }

  // Limit simultaneous borrowed frames when the graph exceeds cache capacity.
  // Nested projection callbacks may borrow a second page; no adjacency cache.
  template <class F> void directional_for(size_t count, F&& f) const {
    size_t concurrency = tbb::this_task_arena::max_concurrency();
    if (db_.multiscale && !fits()) {
      for (size_t c = 0; c < 4; ++c) {
        const auto stats = db_.multiscale->pool().stats(static_cast<page_space_t>(c));
        const auto free = stats.frame_count - stats.pinned_frames;
        if (free) concurrency = std::min(concurrency, std::max<size_t>(1, free / 2));
      }
    }
    tbb::task_arena arena(std::max<size_t>(1, concurrency));
    arena.execute([&] { tbb::parallel_for(size_t(0), count, f); });
  }
  // Page-local loops permit vectorization and early exit without fetching the
  // remainder of a giant. No span or guard escapes the synchronous callback.
  template <class F> static void map_range(neighbor_range_t& range, F&& f) {
    range.visit_chunks([&](auto bytes) {
      for (size_t i = 0; i < bytes.size(); i += 4) {
        v_id_t dst;
        std::memcpy(&dst, bytes.data() + i, 4);
        f(dst);
      }
      return false;
    });
  }
  template <class F> static bool any_range(neighbor_range_t& range, F&& f) {
    bool found = false;
    range.visit_chunks([&](auto bytes) {
      for (size_t i = 0; i < bytes.size(); i += 4) {
        v_id_t dst;
        std::memcpy(&dst, bytes.data() + i, 4);
        if (f(dst)) { found = true; return true; }
      }
      return false;
    });
    return found;
  }
  template <class F> void vertex(v_id_t v, F&& f) const {
    if (undirected_) {
      std::vector<v_id_t> neighbors;
      for (auto direction : {direction_t::out, direction_t::in}) {
        auto range = db_.read_neighbor(v, direction);
        for (auto u : range) if (u != v) neighbors.push_back(u);
      }
      std::sort(neighbors.begin(), neighbors.end());
      neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
      neighbor_range_t range(neighbor_span_t(neighbors.data(), neighbors.size()), nullptr);
      f(v, std::move(range));
    } else if (page_aware_ && db_.multiscale && degrees_[v])
      db_.multiscale->visit_base_group(std::span<const v_id_t>(&v, 1), f);
    else {
      auto range = db_.read_neighbor(v);
      f(v, std::move(range));
    }
  }
  template <class F> void map(v_id_t v, F&& f) const {
    vertex(v, [&](auto, auto&& range) { map_range(range, f); });
  }
  template <class F> bool any(v_id_t v, F&& f) const {
    bool result = false;
    vertex(v, [&](auto, auto&& range) { result = any_range(range, f); });
    return result;
  }
  template <class P, class F> void incoming_if(P&& eligible, F&& f, bool reverse = false) const {
    directional_for(n(), [&](size_t i) {
      const auto v = reverse ? n() - 1 - i : i;
      if (!eligible(v)) return;
      if (undirected_) vertex(v, f);
      else { auto range = db_.read_neighbor(v, direction_t::in); f(v, std::move(range)); }
    });
  }

  template <class F> void incoming(F&& f, bool reverse = false) const {
    incoming_if([](v_id_t) { return true; }, std::forward<F>(f), reverse);
  }
  template <class F> void active_in(std::span<const v_id_t> ids, F&& f) const {
    directional_for(ids.size(), [&](size_t i) {
      auto range = db_.read_neighbor(ids[i], direction_t::in); f(ids[i], std::move(range));
    });
  }
  // Test eligibility before acquiring a frame. Entire inactive pages can be
  // skipped during dense pull; no hidden adjacency cache is introduced.
  template <class P, class F> void vertices_if(P&& eligible, F&& f, bool reverse = false) const {
    if (undirected_ && !fits()) {
      directional_for(n(), [&](v_id_t v) { if (eligible(v)) vertex(v, f); });
      return;
    }
    auto visit = [&](v_id_t v, auto&& range) {
      if (eligible(v)) f(v, std::move(range));
    };
    if (page_aware_ && db_.multiscale) {
      auto order = db_.multiscale->scan_order();
      auto groups = db_.multiscale->scan_groups();
      const auto count = groups.size() - 1;
      if (async_ && !fits()) {
        async_algo::for_pages(*db_.multiscale, count, [&](size_t i) {
          const auto g = reverse ? count - 1 - i : i;
          return order.subspan(groups[g], groups[g + 1] - groups[g]);
        }, eligible, visit, *async_, async_stats_);
      } else tbb::parallel_for(tbb::blocked_range<size_t>(0, count, 16), [&](const auto& r) {
        for (size_t i = r.begin(); i < r.end(); ++i) {
          const auto g = reverse ? count - 1 - i : i;
          auto vertices = order.subspan(groups[g], groups[g + 1] - groups[g]);
          if (std::any_of(vertices.begin(), vertices.end(), eligible))
            db_.multiscale->visit_base_group(vertices, visit);
        }
      });
      tbb::parallel_for(size_t(0), isolates_.size(), [&](size_t i) {
        if (eligible(isolates_[i])) f(isolates_[i], neighbor_range_t{});
      });
    } else {
      tbb::parallel_for(tbb::blocked_range<size_t>(0, n(), 256), [&](const auto& r) {
        for (size_t v = r.begin(); v < r.end(); ++v) if (eligible(v)) vertex(v, f);
      });
    }
  }
  // F(v, consuming_neighbor_range); includes isolates exactly once.
  template <class F> void vertices(F&& f, bool reverse = false) const {
    vertices_if([](v_id_t) { return true; }, std::forward<F>(f), reverse);
  }
  // Reordering a frontier is legal: callbacks are independent and vertex IDs stay unchanged.
  template <class F> void active(std::span<const v_id_t> ids, F&& f, bool reverse = false) const {
    if (undirected_ && !fits()) {
      directional_for(ids.size(), [&](size_t i) { vertex(ids[i], f); });
      return;
    }
    if (ids.size() < 128 && page_aware_) {
      for (auto v : ids) vertex(v, f);
      return;
    }
    if (page_aware_ && db_.multiscale && !fits() && ids.size() > 256) {
      std::vector<std::pair<uint32_t, v_id_t>> sorted(ids.size());
      tbb::parallel_for(size_t(0), ids.size(), [&](size_t i) {
        sorted[i] = {db_.multiscale->location(ids[i]).first.page.raw(), ids[i]};
      });
      tbb::parallel_sort(sorted.begin(), sorted.end());
      std::vector<v_id_t> vertices(ids.size());
      std::vector<size_t> groups;
      for (size_t i = 0; i < ids.size(); ++i) {
        vertices[i] = sorted[i].second;
        if (!i || sorted[i].first != sorted[i - 1].first) groups.push_back(i);
      }
      groups.push_back(ids.size());
      if (async_) {
        async_algo::for_pages(*db_.multiscale, groups.size() - 1, [&](size_t i) {
          const auto g = reverse ? groups.size() - 2 - i : i;
          return std::span<const v_id_t>(vertices).subspan(groups[g], groups[g + 1] - groups[g]);
        }, [](v_id_t) { return true; }, f, *async_, async_stats_);
      } else tbb::parallel_for(tbb::blocked_range<size_t>(0, groups.size() - 1, 8),
                        [&](const auto& r) {
        for (size_t i = r.begin(); i < r.end(); ++i) {
          const auto g = reverse ? groups.size() - 2 - i : i;
          auto group = std::span<const v_id_t>(vertices).subspan(
              groups[g], groups[g + 1] - groups[g]);
          if (!degrees_[group.front()]) {
            for (auto v : group) vertex(v, f);
          } else db_.multiscale->visit_base_group(group, f);
        }
      });
      return;
    }
    tbb::parallel_for(tbb::blocked_range<size_t>(0, ids.size(), 64), [&](const auto& r) {
      for (size_t i = r.begin(); i < r.end(); ++i) vertex(ids[i], f);
    });
  }
private:
  bw_graph_db_t& db_;
  bool page_aware_;
  bool undirected_;
  bool base_fits_;
  std::vector<uint32_t> degrees_;
  std::vector<v_id_t> isolates_;
  uint64_t edges_{0};
  std::optional<async_algo::options_t> async_;
  async_algo::stats_t* async_stats_{nullptr};
};
} // namespace bw_graph::byo
