#include "bw_graph/algo/edge_map.h"
#include "bw_graph/algo/byo.h"
#include "bw_graph/algo/async/algorithms.h"
#include "bw_graph/io/multiscale_csr_page.h"
#include <tbb/global_control.h>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <queue>
#include <random>
#include <numeric>
#include <fstream>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#endif
#include <nlohmann/json.hpp>

using namespace bw_graph;
namespace {
struct quiet_t : std::streambuf {
  std::streambuf* previous{std::cout.rdbuf(this)};
  int overflow(int c) override { return traits_type::not_eof(c); }
  void restore() { if (previous) std::cout.rdbuf(std::exchange(previous, nullptr)); }
  ~quiet_t() { restore(); }
};
struct bfs_f {
  std::vector<v_id_t>& parent;
  std::vector<uint32_t>& distance;
  uint32_t level;
  bool cond(v_id_t v) { return std::atomic_ref(parent[v]).load(std::memory_order_relaxed) == INVALID_VID; }
  bool update_atomic(v_id_t src, v_id_t dst) {
    v_id_t expected = INVALID_VID;
    if (!std::atomic_ref(parent[dst]).compare_exchange_strong(expected, src,
          std::memory_order_acq_rel)) return false;
    distance[dst] = level + 1; return true;
  }
};
byo::bfs_result_t old_bfs(bw_graph_db_t& db, v_id_t source) {
  byo::bfs_result_t result{std::vector<v_id_t>(db.get_vertex_count(), INVALID_VID),
                          std::vector<uint32_t>(db.get_vertex_count(), UINT32_MAX)};
  result.parent[source] = source; result.distance[source] = 0;
  vertex_subset_t frontier(db.get_vertex_count(), source);
  uint32_t level = 0;
  while (!frontier.is_empty()) {
    bfs_f f{result.parent, result.distance, level++};
    auto next = bw_edge_map(db, frontier, f, 20);
    frontier.del(); frontier = std::move(next);
  }
  frontier.del(); return result;
}
// Original atomic-push operator, with the same dangling correction as the port.
std::vector<double> old_pagerank(const byo::graph_t& graph, size_t iterations) {
  const auto n = graph.n();
  auto a = std::make_unique<std::atomic<double>[]>(n);
  auto b = std::make_unique<std::atomic<double>[]>(n);
  std::vector<double> inverse(n);
  bool* bitmap = newA(bool, n);
  for (size_t v = 0; v < n; ++v) {
    a[v].store(1.0 / n, std::memory_order_relaxed);
    b[v].store(0, std::memory_order_relaxed); bitmap[v] = true;
    inverse[v] = graph.degree(v) ? 1.0 / graph.degree(v) : 0;
  }
  vertex_subset_t frontier(n, bitmap);
  struct push_t {
    std::atomic<double>* current; std::atomic<double>* next; const double* inverse;
    bool cond(v_id_t) { return true; }
    bool update_atomic(v_id_t u, v_id_t v) {
      next[v].fetch_add(current[u].load(std::memory_order_relaxed) * inverse[u],
                       std::memory_order_relaxed); return false;
    }
  };
  for (size_t it = 0; it < iterations; ++it) {
    bw_edge_map_no_output(graph.db(), frontier, push_t{a.get(), b.get(), inverse.data()});
    const auto dangling = tbb::parallel_reduce(tbb::blocked_range<size_t>(0, n), 0.0,
        [&](const auto& r, double x) {
          for (size_t v = r.begin(); v < r.end(); ++v) if (!graph.degree(v)) x += a[v].load(std::memory_order_relaxed);
          return x;
        }, std::plus<double>());
    const auto difference = tbb::parallel_reduce(tbb::blocked_range<size_t>(0, n), 0.0,
        [&](const auto& r, double x) {
          for (size_t v = r.begin(); v < r.end(); ++v) {
            const double value = (.15 + .85 * dangling) / n + .85 * b[v].load(std::memory_order_relaxed);
            x += std::abs(a[v].load(std::memory_order_relaxed) - value); b[v].store(value, std::memory_order_relaxed); a[v].store(0, std::memory_order_relaxed);
          }
          return x;
        }, std::plus<double>());
    (void)difference; a.swap(b);
  }
  frontier.del();
  std::vector<double> result(n);
  for (size_t v = 0; v < n; ++v) result[v] = a[v].load(std::memory_order_relaxed);
  return result;
}
struct observation_t { double seconds; uint64_t reads, bytes; };
uint64_t reads(bw_graph_db_t& db) {
  if (!db.multiscale) return db.buffer_pool->get_stats().cache_misses;
  uint64_t n = 0;
  for (size_t c = 0; c < 4; ++c)
    n += db.multiscale->pool().stats(static_cast<page_space_t>(c)).disk_reads;
  return n;
}
uint64_t read_bytes(bw_graph_db_t& db) {
  if (!db.multiscale) return reads(db) * BW_GRAPH_PAGE_SIZE;
  uint64_t n = 0;
  for (size_t c = 0; c < 4; ++c) {
    auto space = static_cast<page_space_t>(c);
    n += db.multiscale->pool().stats(space).disk_reads * db.multiscale->disk().page_size(space);
  }
  return n;
}
}
int main(int argc, char** argv) {
  try {
    if (argc < 3) throw std::invalid_argument("Usage: repo config [MiB=64] [rounds=3] [threads=15] old|reference|byo|acgraph|acgraph-sync|hybrid|page|page-io|page-packed samples.json bfs|cc|pagerank [iterations=20] [distance-window=8] [frames|charge]");
    const double mib = argc > 3 ? std::stod(argv[3]) : 32;
    const size_t rounds = argc > 4 ? std::stoul(argv[4]) : 3;
    const size_t threads = argc > 5 ? std::stoul(argv[5]) : 15;
    const std::string mode = argc > 6 ? argv[6] : "byo";
    const std::string algorithm = argc > 8 ? argv[8] : "bfs";
    const size_t iterations = argc > 9 ? std::stoul(argv[9]) : 20;
    if (mode != "old" && mode != "reference" && mode != "byo" &&
        mode != "acgraph" && mode != "acgraph-sync" && mode != "hybrid" &&
        mode != "page" && mode != "page-io" && mode != "page-packed")
      throw std::invalid_argument("Invalid mode");
    if (algorithm != "bfs" && algorithm != "cc" && algorithm != "pagerank")
      throw std::invalid_argument("Invalid algorithm");
    if ((mode == "acgraph" || mode == "acgraph-sync") && algorithm == "pagerank")
      throw std::invalid_argument("Residual PR is not equivalent to fixed-iteration Jacobi PR");
    if ((mode == "page" || mode == "page-io" || mode == "page-packed") && algorithm != "bfs")
      throw std::invalid_argument("Page closure currently supports BFS");
    if (!std::isfinite(mib) || mib <= 0 || !rounds || !threads) throw std::invalid_argument("Positive arguments required");
    std::filesystem::current_path(argv[1]);
    quiet_t quiet;
    load_yaml_config(argv[2]);
    if (!BW_MULTISCALE_ENGINE && (mode == "acgraph" || mode == "acgraph-sync" ||
        mode == "hybrid" || mode == "page" || mode == "page-io" || mode == "page-packed"))
      throw std::invalid_argument("Async comparisons require storage.engine: multiscale");
    tbb::global_control limit(tbb::global_control::max_allowed_parallelism, threads);
    BW_GRAPH_CONSOLIDATION_WORKER_COUNT = 0;
    BW_MULTISCALE_FULL_GIANT_WARMUP = true;
    BW_DELTA_FLUSH_INTERVAL_MS = BW_CSR_FLUSH_INTERVAL_MS = 0;
    BW_DELTA_BUFFER_CHUNK_COUNT = 2; BW_DELTA_BUFFER_CHUNK_SIZE = 32;
    const uint64_t requested_budget = mib * 1048576;
    uint64_t budget = requested_budget, auxiliary_reserve = 0, vertices = 0;
    const bool charge_auxiliary = argc > 11 && std::string(argv[11]) == "charge";
    uint64_t giant_payload = 0, edges = 0;
    std::vector<std::vector<int32_t>> reference;
    std::vector<v_id_t> sources, cc_reference;
    std::vector<double> rank_reference;
    // Independent reference; adjacency memory is destroyed before opening/timing the DB.
    {
      mem_sub_com_t graph("data/" + GRAPH_NAME + ".graph", true, LOAD_BUFFER_SIZE, true, BW_MULTISCALE_ENGINE);
      edges = graph.edge_count(); vertices = graph.vertex_count();
      std::mt19937 random(42);
      std::uniform_int_distribution<v_id_t> source(0, graph.vertex_count() - 1);
      for (size_t r = 0; r < rounds; ++r) {
        auto root = source(random); sources.push_back(root);
        if (algorithm != "bfs") continue;
        reference.emplace_back(graph.vertex_count(), -1);
        auto& distance = reference.back(); distance[root] = 0;
        std::vector<v_id_t> queue{root};
        for (size_t i = 0; i < queue.size(); ++i)
          for (auto dst : graph.get_neighbors_span(queue[i]))
            if (distance[dst] < 0) { distance[dst] = distance[queue[i]] + 1; queue.push_back(dst); }
      }
      if (algorithm == "cc") {
        cc_reference.resize(graph.vertex_count());
        std::iota(cc_reference.begin(), cc_reference.end(), 0);
        auto find = [&](v_id_t v) {
          while (cc_reference[v] != v) { cc_reference[v] = cc_reference[cc_reference[v]]; v = cc_reference[v]; }
          return v;
        };
        for (v_id_t v = 0; v < graph.vertex_count(); ++v)
          for (auto u : graph.get_neighbors_span(v)) {
            auto x = find(v), y = find(u);
            if (x != y) cc_reference[std::max(x, y)] = std::min(x, y);
          }
        for (v_id_t v = 0; v < graph.vertex_count(); ++v) cc_reference[v] = find(v);
      }
      if (algorithm == "pagerank") {
        const size_t n = graph.vertex_count();
        rank_reference.assign(n, 1.0 / n);
        std::vector<double> next(n);
        for (size_t it = 0; it < iterations; ++it) {
          double dangling = 0;
          for (v_id_t v = 0; v < n; ++v)
            if (graph.get_neighbors_span(v).empty()) dangling += rank_reference[v];
          std::fill(next.begin(), next.end(), (.15 + .85 * dangling) / n);
          for (v_id_t v = 0; v < n; ++v) {
            auto neighbors = graph.get_neighbors_span(v);
            for (auto u : neighbors) next[u] += .85 * rank_reference[v] / neighbors.size();
          }
          rank_reference.swap(next);
        }
      }
      for (v_id_t v = 0; v < graph.vertex_count(); ++v)
        if (graph.get_neighbors_span(v).size() >= BW_GRAPH_GIANT_DEGREE_BOUND)
          giant_payload += graph.get_neighbors_span(v).size_bytes();
    }
    uint64_t frame_bytes = 0, fixed_giant_bytes = 0;
    if (BW_MULTISCALE_ENGINE) {
      const auto dir = std::filesystem::path(CSR_PREFIX) / (GRAPH_NAME + "_multiscale");
      auto& groups = BW_MULTISCALE_BUFFER_CONFIG.groups;
      for (auto& g : groups) g = {0, 0};
      const std::array<std::string, 4> files{
          "csr_4k.pages", "csr_32k.pages", "csr_256k.pages", "csr_2m.pages"};
      for (size_t c = 1; c < 4; ++c) {
        const size_t size = multiscale_csr_page_t::page_sizes[c];
        const auto n = (std::filesystem::file_size(dir / files[c]) - 4096) / size;
        groups[c] = {n ? 1u : 0u, n}; frame_bytes += n * size;
      }
      if (charge_auxiliary && mode != "byo" && mode != "reference" && mode != "old") {
        uint64_t pages = 0;
        for (size_t c = 0; c < 4; ++c)
          pages += (std::filesystem::file_size(dir / files[c]) - 4096) /
                   multiscale_csr_page_t::page_sizes[c];
        // Conservative reservation, fixed as the frame budget shrinks. It includes
        // scheduler/state vectors and 2 KiB per request for futures/load bookkeeping.
        auxiliary_reserve = 65536 + threads * 4 * 2048;
        if (mode == "page" || mode == "page-io" || mode == "page-packed") auxiliary_reserve += (mode == "page-packed" ? 8 * vertices : (vertices + 7) / 8) + 16 * pages;
        if (mode == "acgraph" || mode == "acgraph-sync")
          auxiliary_reserve += 13 * vertices + 128 * pages + threads * 5120 * sizeof(v_id_t);
        auxiliary_reserve = (auxiliary_reserve + 4095) / 4096 * 4096;
        if (budget <= auxiliary_reserve)
          throw std::runtime_error("Budget below charged auxiliary reservation");
        budget -= auxiliary_reserve;
      }
      if (budget < frame_bytes + threads * 4096)
        throw std::runtime_error("Budget below resident giant pages plus demand window");
      const auto frames = (budget - frame_bytes) / 4096;
      const auto chunks = std::gcd<uint64_t>(8, frames);
      groups[0] = {chunks, frames / chunks};
      frame_bytes += groups[0].chunk_count * groups[0].chunk_size * 4096;
      groups[5] = {2, 32};
    } else {
      if (budget <= giant_payload + 8 * BW_GRAPH_PAGE_SIZE)
        throw std::runtime_error("Budget below legacy giant cache plus demand window");
      BW_BUFFER_CHUNK_COUNT = 8;
      BW_BUFFER_CHUNK_SIZE = (budget - giant_payload) / (8 * BW_GRAPH_PAGE_SIZE);
      frame_bytes = BW_BUFFER_CHUNK_COUNT * BW_BUFFER_CHUNK_SIZE * BW_GRAPH_PAGE_SIZE;
      fixed_giant_bytes = giant_payload;
    }
    bw_graph_db_t db(GRAPH_NAME, false, false, false, BW_GRAPH_ID_AWARE);
    db.total_edge_count_ = edges; // Same complete edge count and divisor on both engines.
    if (db.multiscale) {
      fixed_giant_bytes = db.multiscale->giant_bytes(); // Already inside frames.
      if (frame_bytes < fixed_giant_bytes + threads * 4096)
        throw std::runtime_error("Budget below all giant fragments plus concurrent demand frames");
    }
    auto preparation = std::chrono::steady_clock::now();
    byo::graph_t graph(db, mode != "reference");
    async_algo::stats_t async_stats;
    async_algo::options_t async_options{threads, 4, mode != "acgraph-sync" && mode != "page" && mode != "page-packed"};
    async_options.compact = mode != "page-packed";
    async_options.distance_window = argc > 10 ? std::stoul(argv[10]) : 8;
    if (mode == "hybrid") graph.set_async(async_options, &async_stats);
    std::vector<async_algo::stats_t> scheduler_samples;
    const auto setup_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - preparation).count();
    std::vector<observation_t> times;
    std::vector<double> warmup_seconds, validation_seconds;
    std::vector<uint64_t> output_bytes;
    for (size_t r = 0; r < rounds; ++r) {
      const auto warmup_start = std::chrono::steady_clock::now();
      db.warm_up();
      warmup_seconds.push_back(std::chrono::duration<double>(
          std::chrono::steady_clock::now() - warmup_start).count());
      const auto before_reads = reads(db), before_bytes = read_bytes(db);
      byo::bfs_result_t bfs;
      std::vector<v_id_t> components;
      std::vector<double> rank;
      async_stats = {};
      auto start = std::chrono::steady_clock::now();
      if (mode == "acgraph" || mode == "hybrid" || mode == "page-io") {
        // Keep compute + active I/O workers within the same requested thread budget.
        const auto pool = db.multiscale->pool().stats(page_space_t::CSR_4K);
        async_options.io = !graph.fits() && threads > 4 &&
            pool.frame_count - pool.pinned_frames > threads * 2;
        async_options.workers = async_options.io ? threads - 4 : threads;
        if (mode == "hybrid") graph.set_async(async_options, &async_stats);
      }
      const bool acgraph = mode == "acgraph" || mode == "acgraph-sync";
      if (algorithm == "bfs") bfs = mode == "old" ? old_bfs(db, sources[r]) :
          (mode == "page" || mode == "page-io" || mode == "page-packed") ? async_algo::page_bfs(graph, sources[r], async_options, &async_stats) :
          acgraph ? async_algo::bfs(graph, sources[r], async_options, &async_stats) :
                    byo::bfs(graph, sources[r]);
      else if (algorithm == "cc") {
        if (mode == "old") {
          auto values = db.wcc_sv();
          components.resize(graph.n());
          for (auto [v, c] : values) components[v] = c;
        } else components = acgraph ? async_algo::connected_components(graph, async_options, &async_stats) :
                                     byo::connected_components(graph);
      } else rank = mode == "old" ? old_pagerank(graph, iterations) :
                                   byo::pagerank(graph, iterations, .85, 0).rank;
      const auto elapsed = std::chrono::duration<double>(
          std::chrono::steady_clock::now() - start).count();
      const auto validation_start = std::chrono::steady_clock::now();
      for (v_id_t v = 0; v < graph.n(); ++v) {
        if (algorithm == "bfs") {
          const auto distance = reference[r][v] < 0 ? UINT32_MAX : uint32_t(reference[r][v]);
          if (bfs.distance[v] != distance)
            throw std::runtime_error("BFS reference mismatch: source=" + std::to_string(sources[r]) +
                " vertex=" + std::to_string(v) + " expected=" + std::to_string(distance) +
                " actual=" + std::to_string(bfs.distance[v]));
        } else if (algorithm == "cc") {
          if (components[v] != cc_reference[v]) throw std::runtime_error("CC reference mismatch");
        } else if (!std::isfinite(rank[v]) || std::abs(rank[v] - rank_reference[v]) > 1e-10)
          throw std::runtime_error("PageRank reference mismatch");
      }
      if (charge_auxiliary && async_stats.auxiliary_bytes > auxiliary_reserve)
        throw std::runtime_error("Known auxiliary allocation exceeds charged reservation");
      validation_seconds.push_back(std::chrono::duration<double>(
          std::chrono::steady_clock::now() - validation_start).count());
      output_bytes.push_back(bfs.parent.capacity() * sizeof(v_id_t) +
          bfs.distance.capacity() * sizeof(uint32_t) + components.capacity() * sizeof(v_id_t) +
          rank.capacity() * sizeof(double));
      scheduler_samples.push_back(async_stats);
      times.push_back({elapsed, reads(db) - before_reads, read_bytes(db) - before_bytes});
    }
    if (argc > 7) {
      nlohmann::json samples;
      samples["mode"] = mode; samples["algorithm"] = algorithm;
      samples["adapter_bytes"] = graph.metadata_bytes(); samples["setup_seconds"] = setup_seconds;
      samples["iterations"] = iterations;
      samples["io_workers"] = 4; // Native manager default, identical in every mode.
      samples["async_window_requested"] = async_options.window;
      samples["async_compute_workers"] = async_options.workers;
      samples["async_io_enabled"] = async_options.io &&
          (mode == "acgraph" || mode == "hybrid" || mode == "page-io");
      samples["distance_window"] = async_options.distance_window;
      samples["compact_bfs"] = async_options.compact;
      samples["seed"] = 42; samples["threads"] = threads;
      samples["dataset"] = GRAPH_NAME;
      samples["layout"] = BW_GRAPH_LAYOUT ? graph_layout_name(*BW_GRAPH_LAYOUT) : "legacy-flags";
      samples["frame_bytes"] = frame_bytes;
      uint64_t all_frame_bytes = 0;
      if (db.multiscale) {
        for (size_t c = 0; c < 6; ++c) {
          const auto space = static_cast<page_space_t>(c);
          all_frame_bytes += db.multiscale->pool().stats(space).frame_count *
              uint64_t(db.multiscale->disk().page_size(space));
        }
        samples["all_six_space_frame_bytes"] = all_frame_bytes;
        samples["non_csr_frame_bytes"] = all_frame_bytes - frame_bytes;
        samples["resident_index_accounted_bytes"] = db.multiscale->index_bytes();
      }
      samples["requested_budget_bytes"] = requested_budget;
      samples["charge_auxiliary"] = charge_auxiliary;
      samples["auxiliary_reserve_bytes"] = auxiliary_reserve;
      samples["comparison_envelope_bytes"] = frame_bytes + auxiliary_reserve;
      samples["giant_bytes"] = fixed_giant_bytes;
#if defined(__linux__) || defined(__APPLE__)
      struct rusage usage {};
      if (getrusage(RUSAGE_SELF, &usage) == 0) {
        // Includes import/reference construction and validation, not only timed kernels.
#ifdef __APPLE__
        samples["process_peak_rss_bytes"] = usage.ru_maxrss;
#elif defined(__linux__)
        samples["process_peak_rss_bytes"] = uint64_t(usage.ru_maxrss) * 1024;
#endif
      }
#endif
      for (size_t i = 0; i < times.size(); ++i)
        samples["rounds"].push_back({{"source", sources[i]}, {"seconds", times[i].seconds},
            {"warmup_seconds", warmup_seconds[i]}, {"validation_seconds", validation_seconds[i]},
            {"output_capacity_bytes", output_bytes[i]},
            {"reads", times[i].reads}, {"read_bytes", times[i].bytes},
            {"scheduler_auxiliary_bytes", scheduler_samples[i].auxiliary_bytes},
            {"scheduler_tasks", scheduler_samples[i].tasks},
            {"processed_vertices", scheduler_samples[i].vertices},
            {"processed_edges", scheduler_samples[i].edges},
            {"async_requests", scheduler_samples[i].async_reads},
            {"scheduler_wait_ns", scheduler_samples[i].wait_ns}});
      std::ofstream output(argv[7]);
      output << samples.dump(2) << '\n';
      if (!output) throw std::runtime_error("Cannot write BFS samples");
    }
    std::sort(times.begin(), times.end(), [](auto a, auto b) { return a.seconds < b.seconds; });
    auto median = times[times.size() / 2];
    median.seconds = (times[(times.size() - 1) / 2].seconds + median.seconds) / 2;
    quiet.restore();
    std::cout << std::fixed << std::setprecision(4) << "[pass] "
              << mode << ' ' << algorithm << ' ' << GRAPH_NAME
              << " layout=" << (BW_GRAPH_LAYOUT ? graph_layout_name(*BW_GRAPH_LAYOUT) :
                                  (BW_GRAPH_ID_AWARE ? "id-based" : "legacy-flags"))
              << " data_budget_MiB=" << (frame_bytes + (db.multiscale ? 0 : giant_payload))/1048576.0
              << " auxiliary_reserve_MiB=" << auxiliary_reserve/1048576.0
              << " giant_MiB=" << fixed_giant_bytes/1048576.0
              << " known_index_MiB=" << (db.multiscale ? db.multiscale->index_bytes()/1048576.0 :
                  (db.vertex_index->items.capacity() * sizeof(v_item_t) +
                   db.vertex_index->items.size() * sizeof(vertex_version_t))/1048576.0)
              << " adapter_MiB=" << graph.metadata_bytes()/1048576.0 << " setup_s=" << setup_seconds
              << " threads=" << threads << " rounds=" << rounds << " median_s=" << median.seconds
              << " range_s=" << times.front().seconds << ".." << times.back().seconds
              << " reads=" << median.reads << " read_MiB=" << median.bytes/1048576.0 << '\n';
    { quiet_t close_output; db.close(); }
    return 0;
  } catch (const std::exception& e) { std::cerr << "[fail] " << e.what() << '\n'; }
  catch (...) { std::cerr << "[fail] graph benchmark exception\n"; }
  return 1;
}
