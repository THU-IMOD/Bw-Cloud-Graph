#include "bw_graph/part/louvain.h"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include "louvain.hxx"

namespace bw_graph {
graph_partition_t compute_louvain_communities(mem_sub_com_t& source) {
  const auto start = std::chrono::steady_clock::now();
  const auto n = source.vertex_count();
  if (n >= UINT32_MAX) throw std::overflow_error("Louvain vertex ID range");
  struct omp_settings_t {
    int threads = omp_get_max_threads(), dynamic = omp_get_dynamic();
    ~omp_settings_t() { omp_set_num_threads(threads); omp_set_dynamic(dynamic); }
  } restore;
  omp_set_dynamic(0);
  omp_set_num_threads(std::max(1, omp_get_num_procs()));
  int actual_threads = 1;
#pragma omp parallel
  {
#pragma omp single
    actual_threads = omp_get_num_threads();
  }
  // The dependency uses community zero as a sentinel in local moving. Offset
  // vertex keys by one so original vertex zero participates normally too.
  DiGraph<uint32_t, None, float> graph;
  graph.reserve(size_t(n) + 1);
  for (v_id_t v = 0; v < n; ++v) graph.addVertex(v + 1);
  for (v_id_t v = 0; v < n; ++v)
    for (auto dst : source.get_neighbors_span(v)) graph.addEdge(v + 1, dst + 1, 1.0f);
  graph.update();
  std::vector<uint32_t> membership(size_t(n) + 1);
  if (source.edge_count()) membership = louvainStaticOmp(graph, LouvainOptions(1)).membership;
  else for (v_id_t v = 0; v < n; ++v) membership[v + 1] = v + 1;
  if (membership.size() != size_t(n) + 1) throw std::runtime_error("Invalid Louvain membership");
  // Stable community order by first original vertex encountered, without
  // renumbering the graph's public vertex IDs.
  std::vector<size_t> position(size_t(n) + 1, SIZE_MAX);
  graph_partition_t result;
  for (v_id_t v = 0; v < n; ++v) {
    const auto community = membership[v + 1];
    if (community > n) throw std::runtime_error("Invalid Louvain community ID");
    auto& index = position[community];
    if (index == SIZE_MAX) { index = result.size(); result.emplace_back(); }
    result[index].push_back(v);
  }
  last_louvain_build = {actual_threads, result.size(),
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()};
  return result;
}
} // namespace bw_graph
