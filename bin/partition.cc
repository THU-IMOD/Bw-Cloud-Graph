#include "bw_graph/part/partition.h"

#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/mem/mem_g0com.h"

#include <iomanip>
#include <iostream>
#include <kaminpar.h>
#include <vector>

/* Function Call Experiment */
int main() {
  std::string test_file = "data/yt.graph";
  load_yaml_config("config/yt.yaml");
  uint64_t max_block_weight = bw_graph::BW_GRAPH_MAX_BLOCK_WEIGHT;
  mem_sub_com_t com_graph(test_file, true);

  std::cout << "Graph loaded successfully." << std::endl;
  std::cout << "Number of nodes: " << com_graph.vertex_count() << std::endl;
  std::cout << "Number of edges: " << com_graph.edge_count() << std::endl;

  std::cout << "NodeID size: " << sizeof(kaminpar::shm::NodeID) << " bytes" << std::endl;
  std::cout << "EdgeID size: " << sizeof(kaminpar::shm::EdgeID) << " bytes" << std::endl;

  try {
    const size_t n = com_graph.vertex_count();
    const size_t m = com_graph.edge_count();
    const auto& verts = com_graph.vertex_list();
    // ============================================================
    // Test: absolute weight-constrained partitioning
    // ============================================================
    std::cout << "Start partitioning: "
              << " BW_GRAPH_GIANT_DEGREE_BOUND:" << bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND
              << " max_block_weight:" << max_block_weight << std::endl;
    auto block_of_abs = absolute_weight_graph_partition(
        com_graph,
        static_cast<kaminpar::shm::NodeWeight>(
            max_block_weight)); // This function automatically refines oversized
                                // blocks and compresses IDs

    // --- Statistics: node buckets and partition count
    size_t max_id = 0;
    for (const auto& kv : block_of_abs)
      max_id = std::max(max_id, kv.second);
    size_t B2 = max_id + 1;
    std::vector<std::vector<v_id_t>> buckets2(B2);
    for (const auto& kv : block_of_abs)
      buckets2[kv.second].push_back(kv.first);

    // --- Node counts
    std::vector<size_t> node_counts2(B2, 0);
    for (size_t b = 0; b < B2; ++b)
      node_counts2[b] = buckets2[b].size();

    // --- Weights
    std::vector<kaminpar::shm::BlockWeight> weights2(B2, 0);
    for (size_t b = 0; b < B2; ++b) {
      kaminpar::shm::BlockWeight w = 0;
      for (v_id_t v : buckets2[b])
        w += node_weight_original(com_graph, v);
      weights2[b] = w;
    }

    // --- Edge cut
    size_t cut2 = 0;
    for (size_t i = 0; i < n; ++i) {
      const v_id_t u = verts[i];
      auto it_u = block_of_abs.find(u);
      if (it_u == block_of_abs.end())
        continue;
      const size_t bu = it_u->second;
      for (size_t j = com_graph.offsets[i]; j < com_graph.offsets[i + 1]; ++j) {
        const v_id_t v = com_graph.neighbor_list[j];
        auto it_v = block_of_abs.find(v);
        if (it_v == block_of_abs.end())
          continue;
        const size_t bv = it_v->second;
        if (bu != bv)
          ++cut2;
      }
    }
    cut2 /= 2;

    // --- Node statistics
    std::vector<size_t> nc2_sorted = node_counts2;
    std::sort(nc2_sorted.begin(), nc2_sorted.end());
    size_t min_nodes2 = nc2_sorted.front();
    size_t max_nodes2 = nc2_sorted.back();
    double avg_nodes2 = static_cast<double>(n) / std::max<size_t>(1, B2);
    double median_nodes2 = (B2 % 2 == 0) ? (nc2_sorted[B2 / 2 - 1] + nc2_sorted[B2 / 2]) / 2.0
                                         : static_cast<double>(nc2_sorted[B2 / 2]);

    // --- Weight statistics
    std::vector<kaminpar::shm::BlockWeight> w2_sorted = weights2;
    std::sort(w2_sorted.begin(), w2_sorted.end());
    auto min_weight2 = w2_sorted.front();
    auto max_weight2 = w2_sorted.back();

    // --- Output
    std::cout << "\n=== absolute_weight_graph_partition (max_block_weight = " << max_block_weight
              << ") ===" << std::endl;
    std::cout << "Partitions: " << B2 << std::endl;

    std::cout << "\nNode Distribution Statistics:" << std::endl;
    std::cout << "Average nodes per partition: " << std::fixed << std::setprecision(2) << avg_nodes2
              << std::endl;
    std::cout << "Minimum nodes: " << min_nodes2 << std::endl;
    std::cout << "Maximum nodes: " << max_nodes2 << std::endl;
    std::cout << "Median nodes: " << std::fixed << std::setprecision(2) << median_nodes2
              << std::endl;
    std::cout << "Load imbalance ratio: " << std::fixed << std::setprecision(4)
              << (max_nodes2 / std::max(1e-12, avg_nodes2)) << std::endl;

    std::cout << "\nWeight Distribution Statistics:" << std::endl;
    std::cout << "Minimum weight: " << min_weight2 << std::endl;
    std::cout << "Maximum weight: " << max_weight2 << std::endl;
    std::cout << "Weight imbalance ratio: " << std::fixed << std::setprecision(4)
              << (double) max_weight2 /
                     (double) std::max<kaminpar::shm::BlockWeight>(1, min_weight2)
              << std::endl;

    std::cout << "\nEdge Cut Statistics:" << std::endl;
    std::cout << "Edge cut (verified): " << cut2 << " edges" << std::endl;
    std::cout << "Edge cut ratio: " << std::fixed << std::setprecision(2)
              << (m ? (double) cut2 / (double) m * 100.0 : 0.0) << "%" << std::endl;

    {
      bool ok = true;
      size_t missing_cnt = 0, zero_cnt = 0, out_of_range_cnt = 0;

      for (size_t i = 0; i < n; ++i) {
        const v_id_t u = verts[i];
        auto it = block_of_abs.find(u);
        if (it == block_of_abs.end()) {
          ++missing_cnt;
          ok = false;
        } else if (it->second >= B2) {
          ++out_of_range_cnt;
          ok = false;
        }
      }

      size_t empty_block_cnt = 0;
      for (size_t b = 0; b < B2; ++b) {
        if (buckets2[b].empty())
          ++empty_block_cnt;
      }

      std::cout << "\n=== Equivalence-Class Completeness Check ===" << std::endl;
      std::cout << "All vertices assigned exactly once and block ids "
                   "compressed to [0.."
                << B2 - 1 << "]? " << (ok ? "YES" : "NO") << std::endl;
      if (!ok) {
        std::cout << "  Unassigned vertices: " << missing_cnt << std::endl;
        std::cout << "  Zero-id vertices:    " << zero_cnt << std::endl;
        std::cout << "  Out-of-range ids:    " << out_of_range_cnt << std::endl;
      }
      std::cout << "Empty blocks (if any): " << empty_block_cnt << std::endl;
    }
  } catch (const std::exception& e) {
    std::cerr << "Error during partitioning: " << e.what() << std::endl;
    return 1;
  }

  // ============================================================
  // Test: absolute weight-constrained partitioning with mem_sub_map_t
  // ============================================================
  try {
    std::cout << "\n\n=== Testing mem_sub_map_t ===" << std::endl;

    mem_sub_map_t map_graph;

    // Load yt.graph into a temporary CSR graph
    mem_sub_com_t tmp_csr_graph(test_file, true);

    // Convert CSR to adjacency map
    folly::F14FastMap<v_id_t, std::vector<v_id_t>> adj_map;
    const auto& verts_csr = tmp_csr_graph.vertex_list();
    for (size_t i = 0; i < tmp_csr_graph.vertex_count(); ++i) {
      v_id_t u = verts_csr[i];
      std::vector<v_id_t> nbrs;
      for (size_t j = tmp_csr_graph.offsets[i]; j < tmp_csr_graph.offsets[i + 1]; ++j) {
        nbrs.push_back(tmp_csr_graph.neighbor_list[j]);
      }
      adj_map.emplace(u, std::move(nbrs));
    }

    // Build mem_sub_map_t from adjacency map
    map_graph = mem_sub_map_t(adj_map);

    std::cout << "Graph loaded successfully." << std::endl;
    std::cout << "Number of nodes: " << map_graph.vertex_count() << std::endl;
    std::cout << "Number of edges: " << map_graph.edge_count() << std::endl;

    const size_t n = map_graph.vertex_count();
    const size_t m = map_graph.edge_count();
    const auto& verts = map_graph.vertex_list();

    std::cout << "Start partitioning: "
              << " BW_GRAPH_GIANT_DEGREE_BOUND:" << bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND
              << " max_block_weight:" << max_block_weight << std::endl;

    auto block_of_abs = absolute_weight_graph_partition(
        map_graph, static_cast<kaminpar::shm::NodeWeight>(
                       max_block_weight)); // This function refines oversized
                                           // blocks and compresses IDs

    // --- Statistics: node buckets and partition count
    size_t max_id = 0;
    for (const auto& kv : block_of_abs)
      max_id = std::max(max_id, kv.second);
    size_t B2 = max_id + 1;
    std::vector<std::vector<v_id_t>> buckets2(B2);
    for (const auto& kv : block_of_abs)
      buckets2[kv.second].push_back(kv.first);

    // --- Node counts
    std::vector<size_t> node_counts2(B2, 0);
    for (size_t b = 0; b < B2; ++b)
      node_counts2[b] = buckets2[b].size();

    // --- Weights
    std::vector<kaminpar::shm::BlockWeight> weights2(B2, 0);
    for (size_t b = 0; b < B2; ++b) {
      kaminpar::shm::BlockWeight w = 0;
      for (v_id_t v : buckets2[b]) {
        const auto ng = map_graph.get_neighbors(v);
        const size_t deg = ng.size();
        if (deg >= bw_graph::BW_GRAPH_GIANT_DEGREE_BOUND)
          w += static_cast<kaminpar::shm::NodeWeight>(1);
        else
          w += static_cast<kaminpar::shm::NodeWeight>(1 + deg);
      }
      weights2[b] = w;
    }

    // --- Edge cut
    size_t cut2 = 0;
    for (v_id_t u : verts) {
      auto it_u = block_of_abs.find(u);
      if (it_u == block_of_abs.end())
        continue;
      const size_t bu = it_u->second;
      const auto& nbrs = map_graph.get_neighbors_ref(u);
      for (v_id_t v : nbrs) {
        auto it_v = block_of_abs.find(v);
        if (it_v == block_of_abs.end())
          continue;
        const size_t bv = it_v->second;
        if (bu != bv)
          ++cut2;
      }
    }
    cut2 /= 2; // undirected

    // --- Node statistics
    std::vector<size_t> nc2_sorted = node_counts2;
    std::sort(nc2_sorted.begin(), nc2_sorted.end());
    size_t min_nodes2 = nc2_sorted.front();
    size_t max_nodes2 = nc2_sorted.back();
    double avg_nodes2 = static_cast<double>(n) / std::max<size_t>(1, B2);
    double median_nodes2 = (B2 % 2 == 0) ? (nc2_sorted[B2 / 2 - 1] + nc2_sorted[B2 / 2]) / 2.0
                                         : static_cast<double>(nc2_sorted[B2 / 2]);

    // --- Weight statistics
    std::vector<kaminpar::shm::BlockWeight> w2_sorted = weights2;
    std::sort(w2_sorted.begin(), w2_sorted.end());
    auto min_weight2 = w2_sorted.front();
    auto max_weight2 = w2_sorted.back();

    // --- Output
    std::cout << "\n=== absolute_weight_graph_partition (mem_sub_map_t, "
                 "max_block_weight = "
              << max_block_weight << ") ===" << std::endl;
    std::cout << "Partitions: " << B2 << std::endl;

    std::cout << "\nNode Distribution Statistics:" << std::endl;
    std::cout << "Average nodes per partition: " << std::fixed << std::setprecision(2) << avg_nodes2
              << std::endl;
    std::cout << "Minimum nodes: " << min_nodes2 << std::endl;
    std::cout << "Maximum nodes: " << max_nodes2 << std::endl;
    std::cout << "Median nodes: " << std::fixed << std::setprecision(2) << median_nodes2
              << std::endl;
    std::cout << "Load imbalance ratio: " << std::fixed << std::setprecision(4)
              << (max_nodes2 / std::max(1e-12, avg_nodes2)) << std::endl;

    std::cout << "\nWeight Distribution Statistics:" << std::endl;
    std::cout << "Minimum weight: " << min_weight2 << std::endl;
    std::cout << "Maximum weight: " << max_weight2 << std::endl;
    std::cout << "Weight imbalance ratio: " << std::fixed << std::setprecision(4)
              << (double) max_weight2 /
                     (double) std::max<kaminpar::shm::BlockWeight>(1, min_weight2)
              << std::endl;

    std::cout << "\nEdge Cut Statistics:" << std::endl;
    std::cout << "Edge cut (verified): " << cut2 << " edges" << std::endl;
    std::cout << "Edge cut ratio: " << std::fixed << std::setprecision(2)
              << (m ? (double) cut2 / (double) m * 100.0 : 0.0) << "%" << std::endl;

    {
      bool ok = true;
      size_t missing_cnt = 0, out_of_range_cnt = 0;

      for (v_id_t u : verts) {
        auto it = block_of_abs.find(u);
        if (it == block_of_abs.end()) {
          ++missing_cnt;
          ok = false;
        } else if (it->second >= B2) {
          ++out_of_range_cnt;
          ok = false;
        }
      }

      size_t empty_block_cnt = 0;
      for (size_t b = 0; b < B2; ++b) {
        if (buckets2[b].empty())
          ++empty_block_cnt;
      }

      std::cout << "\n=== Equivalence-Class Completeness Check (mem_sub_map_t) ===" << std::endl;
      std::cout << "All vertices assigned exactly once and block ids "
                   "compressed to [0.."
                << B2 - 1 << "]? " << (ok ? "YES" : "NO") << std::endl;
      if (!ok) {
        std::cout << "  Unassigned vertices: " << missing_cnt << std::endl;
        std::cout << "  Out-of-range ids:    " << out_of_range_cnt << std::endl;
      }
      std::cout << "Empty blocks (if any): " << empty_block_cnt << std::endl;
    }
  } catch (const std::exception& e) {
    std::cerr << "Error during partitioning (mem_sub_map_t): " << e.what() << std::endl;
    return 1;
  }

  return 0;
}