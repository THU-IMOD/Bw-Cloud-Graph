#pragma once
#include "bw_graph/common/type.h"
#include "bw_graph/mem/mem_g0com.h"

namespace bw_graph {
struct louvain_build_stats_t {
  int threads{0};
  size_t communities{0};
  double seconds{0}; // Includes conversion and community detection, not Gorder.
};
// Construction diagnostics are local to the calling thread; no update-path counters.
inline thread_local louvain_build_stats_t last_louvain_build;
// Run once over the entire undirected graph, including giants and isolated vertices.
// Community sizes are unconstrained; the CSR builder enforces page byte capacities.
graph_partition_t compute_louvain_communities(mem_sub_com_t& graph);
} // namespace bw_graph
