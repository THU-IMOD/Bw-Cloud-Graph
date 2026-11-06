#include "bw_graph/algo/edge_map.h"
#include "bw_graph/part/louvain.h"
#include "bw_graph/io/multiscale_csr_page.h"
#include <tbb/global_control.h>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <queue>
#include <random>
#include <fstream>
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
  std::atomic<v_id_t>* parent;
  bool cond(v_id_t v) { return parent[v].load(std::memory_order_relaxed) == INVALID_VID; }
  bool update_atomic(v_id_t src, v_id_t dst) {
    v_id_t expected = INVALID_VID;
    return parent[dst].compare_exchange_strong(expected, src, std::memory_order_acq_rel);
  }
};
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
    if (argc < 3) throw std::invalid_argument("Usage: repo config [MiB=32] [rounds=3] [threads=15] [--build] [samples.json]");
    const double mib = argc > 3 ? std::stod(argv[3]) : 32;
    const size_t rounds = argc > 4 ? std::stoul(argv[4]) : 3;
    const size_t threads = argc > 5 ? std::stoul(argv[5]) : 15;
    const bool build = argc > 6 && std::string(argv[6]) == "--build";
    if (mib <= 0 || !rounds || !threads) throw std::invalid_argument("Positive arguments required");
    std::filesystem::current_path(argv[1]);
    quiet_t quiet;
    load_yaml_config(argv[2]);
    tbb::global_control limit(tbb::global_control::max_allowed_parallelism, threads);
    BW_GRAPH_CONSOLIDATION_WORKER_COUNT = 0;
    BW_MULTISCALE_FULL_GIANT_WARMUP = true;
    BW_DELTA_FLUSH_INTERVAL_MS = BW_CSR_FLUSH_INTERVAL_MS = 0;
    BW_DELTA_BUFFER_CHUNK_COUNT = 2; BW_DELTA_BUFFER_CHUNK_SIZE = 32;
    const uint64_t budget = mib * 1048576;
    uint64_t giant_payload = 0, edges = 0;
    std::vector<std::vector<int32_t>> reference;
    std::vector<v_id_t> sources;
    // Independent reference; adjacency memory is destroyed before opening/timing the DB.
    {
      mem_sub_com_t graph("data/" + GRAPH_NAME + ".graph", true, LOAD_BUFFER_SIZE, true, BW_MULTISCALE_ENGINE);
      edges = graph.edge_count();
      std::mt19937 random(42);
      std::uniform_int_distribution<v_id_t> source(0, graph.vertex_count() - 1);
      for (size_t r = 0; r < rounds; ++r) {
        auto root = source(random); sources.push_back(root);
        reference.emplace_back(graph.vertex_count(), -1);
        auto& distance = reference.back(); distance[root] = 0;
        std::vector<v_id_t> queue{root};
        for (size_t i = 0; i < queue.size(); ++i)
          for (auto dst : graph.get_neighbors_span(queue[i]))
            if (distance[dst] < 0) { distance[dst] = distance[queue[i]] + 1; queue.push_back(dst); }
      }
      for (v_id_t v = 0; v < graph.vertex_count(); ++v)
        if (graph.get_neighbors_span(v).size() >= BW_GRAPH_GIANT_DEGREE_BOUND)
          giant_payload += graph.get_neighbors_span(v).size_bytes();
    }
    last_louvain_build = {};
    const auto build_start = std::chrono::steady_clock::now();
    if (build) {
      // Construction is outside the BFS timer and uses the YAML-selected layout.
      bw_graph_db_t constructed(GRAPH_NAME, true, true, false, BW_GRAPH_ID_AWARE, true, true);
      constructed.close();
    }
    const auto build_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - build_start).count();
    const auto layout_stats = last_louvain_build;
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
      if (budget <= frame_bytes + threads * 4096)
        throw std::runtime_error("Budget below resident giant pages plus demand window");
      groups[0] = {8, (budget - frame_bytes) / (8 * 4096)};
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
    std::vector<observation_t> times;
    for (size_t r = 0; r < rounds; ++r) {
      db.warm_up(); // Refill before EVERY timed run; time and reads are excluded.
      auto parent = std::make_unique<std::atomic<v_id_t>[]>(db.get_vertex_count());
      for (v_id_t v = 0; v < db.get_vertex_count(); ++v) parent[v] = INVALID_VID;
      parent[sources[r]] = sources[r];
      bfs_f f{parent.get()};
      vertex_subset_t frontier(db.get_vertex_count(), sources[r]);
      const auto before_reads = reads(db), before_bytes = read_bytes(db);
      auto start = std::chrono::steady_clock::now();
      while (!frontier.is_empty()) {
        auto next = bw_edge_map(db, frontier, f, 20);
        frontier.del(); frontier = std::move(next);
      }
      frontier.del();
      const auto elapsed = std::chrono::duration<double>(
          std::chrono::steady_clock::now() - start).count();
      for (v_id_t v = 0; v < db.get_vertex_count(); ++v) {
        auto p = parent[v].load();
        const auto& distance = reference[r];
        if ((p != INVALID_VID) != (distance[v] >= 0) ||
            (p != INVALID_VID && v != sources[r] &&
             (p >= distance.size() || distance[p] + 1 != distance[v])))
          throw std::runtime_error("BFS distances differ from independent reference");
      }
      times.push_back({elapsed, reads(db) - before_reads, read_bytes(db) - before_bytes});
    }
    if (argc > 7) {
      nlohmann::json samples;
      samples["seed"] = 42; samples["threads"] = threads;
      samples["dataset"] = GRAPH_NAME;
      samples["layout"] = BW_GRAPH_LAYOUT ? graph_layout_name(*BW_GRAPH_LAYOUT) : "legacy-flags";
      samples["frame_bytes"] = frame_bytes;
      samples["giant_bytes"] = fixed_giant_bytes;
      samples["build_seconds"] = build_seconds;
      samples["louvain_threads"] = layout_stats.threads;
      samples["communities"] = layout_stats.communities;
      for (size_t i = 0; i < times.size(); ++i)
        samples["rounds"].push_back({{"source", sources[i]}, {"seconds", times[i].seconds},
            {"reads", times[i].reads}, {"read_bytes", times[i].bytes}});
      std::ofstream output(argv[7]);
      output << samples.dump(2) << '\n';
      if (!output) throw std::runtime_error("Cannot write BFS samples");
    }
    std::sort(times.begin(), times.end(), [](auto a, auto b) { return a.seconds < b.seconds; });
    auto median = times[times.size() / 2];
    quiet.restore();
    std::cout << std::fixed << std::setprecision(4) << "[pass] "
              << (db.multiscale ? "multiscale" : "legacy") << ' ' << GRAPH_NAME
              << " layout=" << (BW_GRAPH_LAYOUT ? graph_layout_name(*BW_GRAPH_LAYOUT) :
                                  (BW_GRAPH_ID_AWARE ? "id-based" : "legacy-flags"))
              << " build_s=" << build_seconds << " louvain_threads=" << layout_stats.threads
              << " communities=" << layout_stats.communities
              << " data_budget_MiB=" << (frame_bytes + (db.multiscale ? 0 : giant_payload))/1048576.0
              << " giant_MiB=" << fixed_giant_bytes/1048576.0
              << " known_index_MiB=" << (db.multiscale ? db.multiscale->index_bytes()/1048576.0 :
                  (db.vertex_index->items.capacity() * sizeof(v_item_t) +
                   db.vertex_index->items.size() * sizeof(vertex_version_t))/1048576.0)
              << " threads=" << threads << " rounds=" << rounds << " median_s=" << median.seconds
              << " range_s=" << times.front().seconds << ".." << times.back().seconds
              << " reads=" << median.reads << " read_MiB=" << median.bytes/1048576.0 << '\n';
    { quiet_t close_output; db.close(); }
    return 0;
  } catch (const std::exception& e) { std::cerr << "[fail] " << e.what() << '\n'; }
  catch (...) { std::cerr << "[fail] graph benchmark exception\n"; }
  return 1;
}
