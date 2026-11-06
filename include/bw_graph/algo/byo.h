#pragma once
// Adapted from BYO 00f8ead / GBBS (MIT); full notice in BYO_LICENSE.txt.
#include "bw_graph/algo/byo_graph.h"

namespace bw_graph::byo {
struct bfs_result_t {
  std::vector<v_id_t> parent;
  std::vector<uint32_t> distance; // UINT32_MAX for unreachable vertices.
};
struct rank_result_t { std::vector<double> rank; size_t iterations; };
struct ldd_result_t { std::vector<v_id_t> cluster, parent; };
using edge_t = std::pair<v_id_t, v_id_t>;

bfs_result_t bfs(const graph_t& graph, v_id_t source, size_t divisor = 20);
std::vector<v_id_t> connected_components(const graph_t& graph);
// Jacobi PageRank; dangling mass is redistributed uniformly (unlike BYO's
// unchecked zero-degree division). epsilon=0 requests exactly max_iterations.
rank_result_t pagerank(const graph_t& graph, size_t max_iterations = 20,
                       double damping = .85, double epsilon = 1e-4);
std::vector<double> betweenness(const graph_t& graph, v_id_t source);
std::vector<uint32_t> coloring(const graph_t& graph, uint64_t seed = 42, bool largest_first = false);
std::vector<uint64_t> kcore(const graph_t& graph);
std::vector<uint8_t> maximal_independent_set(const graph_t& graph, uint64_t seed = 42);
ldd_result_t low_diameter_decomposition(const graph_t& graph, double beta = .2,
                                       uint64_t seed = 42, bool permute = false);
std::vector<edge_t> spanner(const graph_t& graph, size_t k = 4, uint64_t seed = 42);
uint64_t triangle_count(const graph_t& graph);
// BYO convention: directed adjacency count / vertices = 2 * undirected density.
double approximate_densest_subgraph(const graph_t& graph, double epsilon = .001);
uint64_t map_sum(const graph_t& graph);
// Remote-width 0 is bit-packed; otherwise 1,2,4,8,16,32,64 bytes per vertex.
uint64_t map_remote(const graph_t& graph, size_t width, v_id_t selected = 0);
} // namespace bw_graph::byo
