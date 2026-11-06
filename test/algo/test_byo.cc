#include "bw_graph/algo/byo.h"
#include "bw_graph/algo/async/algorithms.h"
#include <gtest/gtest.h>
#include <tbb/global_control.h>
#include <filesystem>
#include <fstream>
#include <queue>
#include <random>
#include <set>
#include <unistd.h>
using namespace bw_graph;
namespace {
using adjacency_t = std::vector<std::vector<v_id_t>>;
struct fixture_t {
  std::filesystem::path previous = std::filesystem::current_path(), path;
  tbb::global_control threads{tbb::global_control::max_allowed_parallelism, 4};
  std::unique_ptr<bw_graph_db_t> db;
  fixture_t(const adjacency_t& a, size_t frames = 32, size_t workers = 0,
            graph_layout_t layout = graph_layout_t::id_based) {
    char pattern[] = "/tmp/bw-byo-test-XXXXXX";
    path = mkdtemp(pattern);
    std::filesystem::create_directory(path / "data");
    { std::ofstream out(path / "data/check.graph");
      size_t m = 0;
      for (const auto& neighbors : a) m += neighbors.size();
      out << "t " << a.size() << ' ' << m << '\n';
      for (size_t u = 0; u < a.size(); ++u) for (auto v : a[u])
        out << "e " << u << ' ' << v << '\n'; }
    CSR_PREFIX = DELTA_PREFIX = META_PREFIX = (path / "store").string();
    BW_MULTISCALE_ENGINE = true;
    BW_GRAPH_LAYOUT = layout;
    BW_GRAPH_CONSOLIDATION_WORKER_COUNT = workers;
    for (auto& group : BW_MULTISCALE_BUFFER_CONFIG.groups) group = {1, frames};
    std::filesystem::current_path(path);
    db = bw_graph_db_t::bulk_load("check");
  }
  ~fixture_t() {
    db.reset();
    std::filesystem::current_path(previous);
    std::error_code e; std::filesystem::remove_all(path, e);
    BW_MULTISCALE_ENGINE = false; BW_GRAPH_LAYOUT.reset();
  }
};
void edge(adjacency_t& a, v_id_t u, v_id_t v) { a[u].push_back(v); a[v].push_back(u); }
std::vector<uint32_t> distances(const adjacency_t& a, v_id_t s) {
  std::vector<uint32_t> d(a.size(), UINT32_MAX);
  std::vector<v_id_t> q{s}; d[s] = 0;
  for (size_t i = 0; i < q.size(); ++i) for (auto v : a[q[i]])
    if (d[v] == UINT32_MAX) { d[v] = d[q[i]] + 1; q.push_back(v); }
  return d;
}
std::vector<double> brandes(const adjacency_t& a, v_id_t source) {
  auto dist = distances(a, source);
  std::vector<v_id_t> order;
  for (size_t v = 0; v < a.size(); ++v) if (dist[v] != UINT32_MAX) order.push_back(v);
  std::sort(order.begin(), order.end(), [&](auto x, auto y) { return dist[x] < dist[y]; });
  std::vector<double> paths(a.size(), 0), result(a.size(), 0); paths[source] = 1;
  for (auto u : order) for (auto v : a[u]) if (dist[v] == dist[u] + 1) paths[v] += paths[u];
  for (auto i = order.rbegin(); i != order.rend(); ++i)
    for (auto v : a[*i]) if (dist[v] == dist[*i] + 1)
      result[*i] += paths[*i] / paths[v] * (1 + result[v]);
  return result;
}
std::vector<uint64_t> core_reference(const adjacency_t& a) {
  std::vector<uint64_t> degree(a.size()), result(a.size());
  std::vector<bool> removed(a.size());
  for (size_t v = 0; v < a.size(); ++v) degree[v] = a[v].size();
  uint64_t k = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    size_t u = a.size();
    for (size_t v = 0; v < a.size(); ++v)
      if (!removed[v] && (u == a.size() || degree[v] < degree[u])) u = v;
    k = std::max(k, degree[u]); result[u] = k; removed[u] = true;
    for (auto v : a[u]) if (!removed[v]) --degree[v];
  }
  return result;
}
double density_reference(const adjacency_t& a) {
  std::set<v_id_t> remaining;
  for (size_t v = 0; v < a.size(); ++v) remaining.insert(v);
  double best = 0;
  while (!remaining.empty()) {
    std::vector<uint64_t> d(a.size()); size_t m = 0;
    for (auto v : remaining) for (auto u : a[v]) if (remaining.contains(u)) { ++d[v]; ++m; }
    const double density = double(m) / remaining.size(); best = std::max(best, density);
    std::vector<v_id_t> remove;
    for (auto v : remaining) if (d[v] <= density * 1.001) remove.push_back(v);
    for (auto v : remove) remaining.erase(v);
  }
  return best;
}
void check(const adjacency_t& a, const byo::graph_t& graph) {
  const auto n = a.size();
  uint64_t m = 0, map = 0, triangles = 0;
  for (size_t u = 0; u < n; ++u) {
    m += a[u].size();
    for (auto v : a[u]) {
      map += v;
      for (auto w : a[v]) if (u < v && v < w && std::find(a[u].begin(), a[u].end(), w) != a[u].end())
        ++triangles;
    }
  }
  EXPECT_EQ(graph.m(), m); EXPECT_EQ(byo::map_sum(graph), map);
  EXPECT_EQ(byo::triangle_count(graph), triangles);
  EXPECT_EQ(byo::kcore(graph), core_reference(a));
  EXPECT_NEAR(byo::approximate_densest_subgraph(graph), density_reference(a), 1e-12);
  auto cc = byo::connected_components(graph);
  auto colors = byo::coloring(graph);
  auto mis = byo::maximal_independent_set(graph);
  for (size_t v = 0; v < n; ++v) {
    auto d = distances(a, v);
    auto traversal = byo::bfs(graph, v);
    EXPECT_EQ(traversal.distance, d);
    for (size_t u = 0; u < n; ++u) {
      if (u != v && d[u] != UINT32_MAX) {
        EXPECT_NE(std::find(a[u].begin(), a[u].end(), traversal.parent[u]), a[u].end());
      }
    }
    auto bc = byo::betweenness(graph, v), ref = brandes(a, v);
    for (size_t u = 0; u < n; ++u) {
      EXPECT_NEAR(bc[u], ref[u], 1e-9);
      EXPECT_EQ(cc[u] == cc[v], d[u] != UINT32_MAX);
    }
    bool dominated = mis[v];
    EXPECT_NE(colors[v], UINT32_MAX);
    for (auto u : a[v]) {
      EXPECT_NE(colors[u], colors[v]);
      EXPECT_FALSE(mis[u] && mis[v]); dominated |= mis[u];
    }
    EXPECT_TRUE(dominated);
  }
  for (bool permute : {false, true}) {
    auto ldd = byo::low_diameter_decomposition(graph, .2, 42, permute);
    for (size_t v = 0; v < n; ++v) {
      ASSERT_LT(ldd.cluster[v], n); ASSERT_LT(ldd.parent[v], n);
      EXPECT_EQ(ldd.cluster[v], ldd.cluster[ldd.parent[v]]);
      auto u = v; size_t steps = 0;
      while (ldd.parent[u] != u && steps++ < n) {
        EXPECT_NE(std::find(a[u].begin(), a[u].end(), ldd.parent[u]), a[u].end());
        u = ldd.parent[u];
      }
      EXPECT_LT(steps, n); EXPECT_EQ(u, ldd.cluster[v]);
    }
  }
  adjacency_t span(n);
  for (auto [u, v] : byo::spanner(graph)) {
    ASSERT_LT(u, n); ASSERT_LT(v, n);
    EXPECT_NE(std::find(a[u].begin(), a[u].end(), v), a[u].end()); edge(span, u, v);
  }
  for (size_t v = 0; v < n; ++v) {
    auto before = distances(a, v), after = distances(span, v);
    for (size_t u = 0; u < n; ++u) EXPECT_EQ(before[u] == UINT32_MAX, after[u] == UINT32_MAX);
  }
  std::vector<double> rank(n, n ? 1.0 / n : 0), next(n);
  for (size_t it = 0; it < 17; ++it) {
    double dangling = 0;
    for (size_t v = 0; v < n; ++v) if (a[v].empty()) dangling += rank[v];
    std::fill(next.begin(), next.end(), n ? (.15 + .85 * dangling) / n : 0);
    for (size_t v = 0; v < n; ++v) for (auto u : a[v]) next[u] += .85 * rank[v] / a[v].size();
    rank.swap(next);
  }
  auto pr = byo::pagerank(graph, 17, .85, 0);
  EXPECT_EQ(pr.iterations, n ? 17u : 0u);
  for (size_t v = 0; v < n; ++v) EXPECT_NEAR(pr.rank[v], rank[v], 1e-11);
  if (n) {
    for (auto width : {0, 1, 2, 4, 8, 16, 32, 64}) {
      EXPECT_EQ(byo::map_remote(graph, width, n / 2), a[n / 2].size());
    }
  }
}
TEST(Byo, AllUnweightedAlgorithmsAgainstIndependentReferences) {
  for (unsigned seed : {7, 19, 43}) {
    adjacency_t a(28); std::mt19937 random(seed);
    for (v_id_t u = 0; u < 24; ++u) for (v_id_t v = u + 1; v < 24; ++v)
      if (random() % 5 == 0 && u / 12 == v / 12) edge(a, u, v);
    fixture_t fixture(a, 32, 0, seed == 7 ? graph_layout_t::id_based :
        seed == 19 ? graph_layout_t::kaminpar_gorder : graph_layout_t::louvain_gorder);
    for (bool optimized : {false, true}) check(a, byo::graph_t(*fixture.db, optimized));
  }
}
TEST(Byo, IsolatesAndEmptyGraph) {
  for (size_t n : {0u, 1u, 12u}) {
    adjacency_t a(n); fixture_t fixture(a); byo::graph_t graph(*fixture.db);
    check(a, graph);
    EXPECT_THROW(byo::bfs(graph, n), std::out_of_range);
    EXPECT_THROW(byo::pagerank(graph, 20, 1), std::invalid_argument);
    EXPECT_THROW(byo::low_diameter_decomposition(graph, 0), std::invalid_argument);
  }
}
TEST(Byo, RebuildAdapterAfterSmoUsesPublishedBaseGeneration) {
  adjacency_t a(20); edge(a, 0, 1);
  fixture_t fixture(a, 16, 2);
  ASSERT_TRUE(fixture.db->insert_edge(0, 2));
  ASSERT_TRUE(fixture.db->insert_edge(2, 0));
  fixture.db->consolidate_all_pages();
  edge(a, 0, 2);
  EXPECT_EQ(fixture.db->multiscale->base_degree(0), 2u);
  check(a, byo::graph_t(*fixture.db));
}
TEST(Byo, TinyPoolGiantChunksEarlyExitAndBaseOnlyDegree) {
  adjacency_t a(10030);
  for (v_id_t v = 1; v <= 10000; ++v) edge(a, 0, v);
  fixture_t fixture(a, 8); byo::graph_t graph(*fixture.db);
  EXPECT_EQ(byo::bfs(graph, 1).distance, distances(a, 1));
  EXPECT_EQ(byo::triangle_count(graph), 0u);
  auto range = fixture.db->read_neighbor(0);
  size_t chunks = 0, count = 0;
  range.visit_chunks([&](auto bytes) { ++chunks; count += bytes.size() / 4; return false; });
  EXPECT_GT(chunks, 1u); EXPECT_EQ(count, 10000u);
  auto early = fixture.db->read_neighbor(0);
  chunks = 0;
  early.visit_chunks([&](auto) { ++chunks; return true; });
  EXPECT_EQ(chunks, 1u);
  ASSERT_TRUE(fixture.db->insert_edge(0, 10001));
  EXPECT_EQ(fixture.db->multiscale->base_degree(0), 10000u);
  EXPECT_EQ(fixture.db->get_vertex_degree(0), 10001u);
  byo::graph_t base(*fixture.db);
  EXPECT_EQ(base.degree(0), 10000u);
}
} // namespace

namespace {
// Keep independent distance/parent and component checks for asynchronous relaxation.
TEST(Byo, AsyncRelaxationAndIoHybrid) {
  std::mt19937 random(17);
  adjacency_t a(1800);
  for (v_id_t v = 1; v < 1700; ++v) edge(a, v - 1, v);
  for (size_t i = 0; i < 18000; ++i) {
    v_id_t u = random() % 1700, v = random() % 1700;
    if (u != v && std::find(a[u].begin(), a[u].end(), v) == a[u].end()) edge(a, u, v);
  }
  for (auto layout : {graph_layout_t::id_based, graph_layout_t::kaminpar_gorder,
                      graph_layout_t::louvain_gorder}) {
    fixture_t fixture(a, 16, 0, layout);
    byo::graph_t graph(*fixture.db);
    auto expected = distances(a, 0);
    auto components = byo::connected_components(graph);
    auto rank = byo::pagerank(graph, 8, .85, 0).rank;
    for (bool io : {false, true}) {
      async_algo::stats_t stats;
      auto r = async_algo::bfs(graph, 0, {4, 2, io}, &stats);
      EXPECT_EQ(r.distance, expected);
      for (size_t v = 1; v < 1700; ++v) {
        ASSERT_LT(r.parent[v], a.size());
        EXPECT_EQ(r.distance[r.parent[v]] + 1, r.distance[v]);
        EXPECT_NE(std::find(a[v].begin(), a[v].end(), r.parent[v]), a[v].end());
      }
      EXPECT_EQ(async_algo::connected_components(graph, {4, 2, io}), components);
      for (uint32_t window : {0, 1, 8}) for (bool compact : {false, true}) {
        auto page_result = async_algo::page_bfs(graph, 0, {4, 2, io, window, compact});
        EXPECT_EQ(page_result.distance, expected);
        for (size_t v = 1; v < 1700; ++v) {
          ASSERT_LT(page_result.parent[v], a.size());
          EXPECT_EQ(page_result.distance[page_result.parent[v]] + 1, page_result.distance[v]);
          EXPECT_NE(std::find(a[v].begin(), a[v].end(), page_result.parent[v]), a[v].end());
        }
      }
      EXPECT_EQ(async_algo::bfs(graph, 1799, {4, 2, io}).distance, distances(a, 1799));
    }
    async_algo::stats_t stats;
    graph.set_async({4, 2, true}, &stats);
    EXPECT_EQ(byo::bfs(graph, 0).distance, expected);
    EXPECT_EQ(byo::connected_components(graph), components);
    auto hybrid_rank = byo::pagerank(graph, 8, .85, 0).rank;
    for (size_t v = 0; v < a.size(); ++v) EXPECT_NEAR(rank[v], hybrid_rank[v], 1e-12);
    EXPECT_GT(stats.async_reads, 0);
    // Drained futures must not leave pins outside the retained immutable warmup set.
    EXPECT_EQ(fixture.db->multiscale->pool().stats(page_space_t::CSR_4K).pinned_frames, 0);
  }
}

} // namespace

namespace {
TEST(Byo, HybridAllKernelsAndExceptionDrain) {
  adjacency_t a(64);
  for (v_id_t u = 0; u < a.size(); ++u)
    for (v_id_t v = u + 1; v < a.size(); ++v) edge(a, u, v);
  fixture_t fixture(a, 3);
  byo::graph_t graph(*fixture.db);
  async_algo::stats_t stats;
  graph.set_async({1, 1, true}, &stats);
  check(a, graph);
  EXPECT_GT(stats.async_reads, 0);
  EXPECT_THROW(graph.vertices([](v_id_t, auto&&) { throw std::runtime_error("callback failure"); }),
               std::runtime_error);
  EXPECT_EQ(fixture.db->multiscale->pool().stats(page_space_t::CSR_4K).pinned_frames, 0);
  EXPECT_EQ(byo::bfs(graph, 0).distance, distances(a, 0));
  EXPECT_THROW(graph.set_async({0, 1, true}), std::invalid_argument);
  EXPECT_THROW(async_algo::bfs(graph, 64, {1, 1, true}), std::out_of_range);
}
}

namespace {
TEST(Byo, PageActivationPublishesEveryConcurrentImprovement) {
  adjacency_t a(6000);
  for (v_id_t v = 1; v < a.size(); ++v) edge(a, v - 1, v);
  std::mt19937 random(181);
  for (size_t i = 0; i < 18000; ++i) {
    v_id_t u = random() % a.size(), v = (u + random() % 128) % a.size();
    if (u != v && std::find(a[u].begin(), a[u].end(), v) == a[u].end()) edge(a, u, v);
  }
  for (v_id_t v = 3000; v < 4500; ++v) edge(a, 0, v);
  fixture_t fixture(a, 8, 0, graph_layout_t::kaminpar_gorder);
  byo::graph_t graph(*fixture.db);
  for (v_id_t source : {5999, 17, 2201, 4501, 3071, 9}) {
    const auto expected = distances(a, source);
    for (size_t repeat = 0; repeat < 3; ++repeat) {
      const auto result = async_algo::page_bfs(graph, source, {4, 2, true, 8});
      ASSERT_EQ(result.distance, expected);
      for (size_t v = 0; v < a.size(); ++v) if (v != source) {
        ASSERT_LT(result.parent[v], a.size());
        EXPECT_EQ(result.distance[result.parent[v]] + 1, result.distance[v]);
      }
    }
  }
}
}

namespace {
TEST(Byo, AsymmetricDirectedAlgorithmsAndDensePull) {
  adjacency_t a(240);
  // Dense first frontier, one-way chain, sink, disconnected vertex, and a back arc.
  for (v_id_t v = 1; v < 100; ++v) a[0].push_back(v);
  for (v_id_t v = 1; v < 220; ++v) a[v].push_back(v + 1);
  a[230].push_back(12);
  fixture_t fixture(a, 32);
  byo::graph_t graph(*fixture.db);
  const auto expected = distances(a, 0);
  EXPECT_EQ(byo::bfs(graph, 0).distance, expected);
  EXPECT_EQ(byo::bfs(graph, 221).distance, distances(a, 221));
  async_algo::options_t opts; opts.workers = 2;
  EXPECT_EQ(async_algo::bfs(graph, 0, opts).distance, expected);
  auto cc = byo::connected_components(graph);
  EXPECT_EQ(cc[0], cc[230]); EXPECT_NE(cc[0], cc[239]);
  EXPECT_EQ(async_algo::connected_components(graph, opts), cc);
  std::vector<double> rank(a.size(), 1.0 / a.size()), next(a.size());
  for (unsigned iteration = 0; iteration < 20; ++iteration) {
    double dangling = 0;
    for (size_t v = 0; v < a.size(); ++v) if (a[v].empty()) dangling += rank[v];
    std::fill(next.begin(), next.end(), (.15 + .85 * dangling) / a.size());
    for (size_t v = 0; v < a.size(); ++v)
      for (auto u : a[v]) next[u] += .85 * rank[v] / a[v].size();
    rank.swap(next);
  }
  for (const auto& result : {fixture.db->wcc_sv(), fixture.db->wcc_map(), fixture.db->wcc()})
    for (auto [v, label] : result) EXPECT_EQ(label, cc[v]);
  for (const auto& result : {fixture.db->parallel_pagerank_map(20, .85, 0),
                             fixture.db->pagerank(20, .85, 0)})
    for (auto [v, value] : result) EXPECT_NEAR(value, rank[v], 1e-12);
  for (auto [v, distance] : fixture.db->parallel_bfs_map(0)) EXPECT_EQ(distance, expected[v]);
  EXPECT_EQ(async_algo::page_bfs(graph, 0, opts).distance, expected);
  auto actual = byo::pagerank(graph, 20, .85, 0);
  for (size_t v = 0; v < a.size(); ++v) EXPECT_NEAR(actual.rank[v], rank[v], 1e-12);
  auto bc = byo::betweenness(graph, 0), ref = brandes(a, 0);
  for (size_t v = 0; v < a.size(); ++v) EXPECT_NEAR(bc[v], ref[v], 1e-9);
  EXPECT_TRUE(fixture.db->read_neighbor_clone(0, direction_t::in).empty());
  EXPECT_EQ(fixture.db->read_neighbor_clone(1, direction_t::in), (std::vector<v_id_t>{0}));
}

}

namespace {
TEST(Byo, DirectedColdStartAndReopenAlgorithms) {
  fixture_t fixture(adjacency_t{}, 32, 2);
  fixture.db.reset();
  fixture.db = bw_graph_db_t::create_empty("cold_directed");
  {
    byo::graph_t empty(*fixture.db);
    EXPECT_TRUE(byo::connected_components(empty).empty());
    EXPECT_TRUE(byo::pagerank(empty).rank.empty());
  }
  for (unsigned i = 0; i < 5; ++i) EXPECT_EQ(fixture.db->append_vertex(), i);
  ASSERT_TRUE(fixture.db->insert_edge(2, 0));
  ASSERT_TRUE(fixture.db->insert_edge(0, 1));
  ASSERT_TRUE(fixture.db->insert_edge(1, 3));
  fixture.db->consolidate_all_pages();
  for (unsigned reopen = 0; reopen < 2; ++reopen) {
    byo::graph_t graph(*fixture.db);
    EXPECT_EQ(byo::bfs(graph, 2).distance,
              (std::vector<uint32_t>{1, 2, 0, 3, UINT32_MAX}));
    EXPECT_EQ(byo::connected_components(graph), (std::vector<v_id_t>{0, 0, 0, 0, 4}));
    std::vector<double> rank(5, .2), next(5);
    for (unsigned i = 0; i < 20; ++i) {
      std::fill(next.begin(), next.end(), (.15 + .85 * (rank[3] + rank[4])) / 5);
      next[0] += .85 * rank[2]; next[1] += .85 * rank[0]; next[3] += .85 * rank[1];
      rank.swap(next);
    }
    auto actual = byo::pagerank(graph, 20, .85, 0).rank;
    for (unsigned v = 0; v < 5; ++v) EXPECT_NEAR(actual[v], rank[v], 1e-12);
    if (!reopen) { fixture.db.reset(); fixture.db = bw_graph_db_t::open("cold_directed"); }
  }
}
}
