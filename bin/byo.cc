#include "bw_graph/algo/byo.h"
#include "bw_graph/algo/async/algorithms.h"
#include <tbb/global_control.h>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
using namespace bw_graph;
namespace {
struct quiet_t : std::streambuf {
  std::streambuf* old = std::cout.rdbuf(this);
  int overflow(int c) override { return traits_type::not_eof(c); }
  ~quiet_t() { std::cout.rdbuf(old); }
};
}
int main(int argc, char** argv) {
  try {
    if (argc < 3) throw std::invalid_argument(
        "Usage: byo config.yaml [async-|hybrid-|page-|page-io-]bfs|cc|pagerank|bc|coloring|kcore|mis|ldd|spanner|triangle|densest|map|map-remote|all [source=0] [threads=15] [iterations=20] [remote-width=8]");
    load_yaml_config(argv[1]);
    std::string requested = argv[2];
    const bool page_io = requested == "page-io-bfs";
    const bool page = page_io || requested == "page-bfs";
    if (page) requested = "bfs";
    const bool hybrid = requested.starts_with("hybrid-");
    const bool asynchronous = requested.starts_with("async-");
    if (hybrid) requested.erase(0, 7);
    if (asynchronous) requested.erase(0, 6);
    if (asynchronous && requested != "bfs" && requested != "cc")
      throw std::invalid_argument("Async relaxation currently supports bfs and cc");
    const auto source = argc > 3 ? std::stoul(argv[3]) : 0;
    const auto threads = argc > 4 ? std::stoul(argv[4]) : 15;
    const auto iterations = argc > 5 ? std::stoul(argv[5]) : 20;
    const auto width = argc > 6 ? std::stoul(argv[6]) : 8;
    if (!threads) throw std::invalid_argument("Positive thread count required");
    tbb::global_control limit(tbb::global_control::max_allowed_parallelism, threads);
    BW_GRAPH_CONSOLIDATION_WORKER_COUNT = 0;
    std::unique_ptr<bw_graph_db_t> db;
    { quiet_t quiet;
      db = std::make_unique<bw_graph_db_t>(GRAPH_NAME, BW_GRAPH_REBUILD_INDEX,
          BW_GRAPH_REDO_PARTITION, BW_GRAPH_NEIGHBOR_COMPRESS, BW_GRAPH_ID_AWARE, true, true);
      db->warm_up(); }
    byo::graph_t graph(*db);
    async_algo::options_t async_options{threads, 4, false};
    if (db->multiscale && (hybrid || asynchronous || page_io)) {
      auto pool = db->multiscale->pool().stats(page_space_t::CSR_4K);
      async_options.io = !graph.fits() && threads > 4 &&
          pool.frame_count - pool.pinned_frames > threads * 2;
      if (async_options.io) async_options.workers = threads - 4;
    }
    if (hybrid) graph.set_async(async_options);
    const std::vector<std::string> algorithms = requested == "all" ?
        std::vector<std::string>{"bfs", "cc", "pagerank", "bc", "coloring", "kcore", "mis",
                                "ldd", "spanner", "triangle", "densest", "map", "map-remote-0",
                                "map-remote-1", "map-remote-2", "map-remote-4", "map-remote-8",
                                "map-remote-16", "map-remote-32", "map-remote-64"} :
        std::vector<std::string>{requested};
    for (const auto& algorithm : algorithms) {
      { quiet_t quiet; db->warm_up(); }
      double checksum = 0;
      const auto start = std::chrono::steady_clock::now();
      if (algorithm == "bfs") {
        auto r = page ? async_algo::page_bfs(graph, source, async_options) :
                 asynchronous ? async_algo::bfs(graph, source, async_options) : byo::bfs(graph, source);
        checksum = std::count_if(r.distance.begin(), r.distance.end(), [](auto x) { return x != UINT32_MAX; });
      } else if (algorithm == "cc") {
        auto r = asynchronous ? async_algo::connected_components(graph, async_options) : byo::connected_components(graph);
        for (size_t v = 0; v < r.size(); ++v) checksum += r[v] == v;
      } else if (algorithm == "pagerank") {
        auto r = byo::pagerank(graph, iterations, .85, 0);
        checksum = std::accumulate(r.rank.begin(), r.rank.end(), 0.0);
      } else if (algorithm == "bc") {
        auto r = byo::betweenness(graph, source); checksum = std::accumulate(r.begin(), r.end(), 0.0);
      } else if (algorithm == "coloring") {
        auto r = byo::coloring(graph); checksum = r.empty() ? 0 : *std::max_element(r.begin(), r.end()) + 1;
      } else if (algorithm == "kcore") {
        auto r = byo::kcore(graph); checksum = r.empty() ? 0 : *std::max_element(r.begin(), r.end());
      } else if (algorithm == "mis") {
        auto r = byo::maximal_independent_set(graph); checksum = std::count(r.begin(), r.end(), 1);
      } else if (algorithm == "ldd") {
        auto r = byo::low_diameter_decomposition(graph);
        for (size_t v = 0; v < r.cluster.size(); ++v) checksum += r.cluster[v] == v;
      } else if (algorithm == "spanner") checksum = byo::spanner(graph).size();
      else if (algorithm == "triangle") checksum = byo::triangle_count(graph);
      else if (algorithm == "densest") checksum = byo::approximate_densest_subgraph(graph);
      else if (algorithm == "map") checksum = byo::map_sum(graph);
      else if (algorithm == "map-remote") checksum = byo::map_remote(graph, width, source);
      else if (algorithm.starts_with("map-remote-"))
        checksum = byo::map_remote(graph, std::stoul(algorithm.substr(11)), source);
      else throw std::invalid_argument("Unknown BYO algorithm");
      const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      std::cout << std::fixed << std::setprecision(6) << "[pass] " << (page_io ? "page-io-" : page ? "page-" : hybrid ? "hybrid-" : asynchronous ? "async-" : "") << algorithm
                << " seconds=" << seconds << " checksum=" << checksum << '\n';
    }
    { quiet_t quiet; db->close(); }
    return 0;
  } catch (const std::exception& e) { std::cerr << "[fail] " << e.what() << '\n'; }
  catch (...) { std::cerr << "[fail] BYO execution failed\n"; }
  return 1;
}
