#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  std::string config_path = "config/default.yaml";

  if (argc > 1) {
    config_path = argv[1];
    std::cout << "Config path: " << config_path << std::endl;
    load_yaml_config(config_path);
  } else {
    std::cout << "Use Defalult Configuration." << std::endl;
    load_yaml_config();
  }

  print_config();

  std::string g_name = bw_graph::GRAPH_NAME;

  // Load input graph file
  std::string test_file = "data/" + g_name + ".graph";
  mem_sub_com_t com_graph(test_file, true);

  std::cout << "Ground Truth Graph loaded: " << com_graph.vertex_count() << " vertices, "
            << com_graph.edge_count() << " edges" << std::endl;
  // Extract graph dimensions
  size_t n = com_graph.vertex_count();

  std::cout << "Creating database.." << std::endl;
  auto db = bw_graph_db_t(g_name, true, false, false, true);
  assert(db.graph_name_ == g_name);
  std::cout << "Database created successfully" << std::endl;

  std::cout << "Loading all Neighbor Testing" << std::endl;
  // Store neighbor spans for all vertices
  std::vector<std::vector<v_id_t>> page_based_all_neighbors(n);
  for (v_id_t vertex_id = 0; vertex_id < n; ++vertex_id) {
    auto neighbors = db.read_neighbor_clone(vertex_id);
    page_based_all_neighbors[vertex_id] = neighbors;
  }

  std::cout << "\n=== Neighbor Count Verification ===" << std::endl;
  for (v_id_t vertex_id = 0; vertex_id < n; ++vertex_id) {
    auto page_neighbors = page_based_all_neighbors[vertex_id];
    auto ground_truth = com_graph.get_neighbors(vertex_id);
    if (page_neighbors.size() != ground_truth.size()) {
      std::cerr << "Mismatch type: " << db.vertex_index->items[vertex_id].vertex_type << std::endl;
      std::cerr << "Mismatch at vertex " << vertex_id << ": "
                << "Ground truth size = " << ground_truth.size()
                << ", Page-based size = " << page_neighbors.size() << std::endl;
    }
  }

  // Correctness validation using random sampling
  std::cout << "\n=== Correctness Verification Sample ===" << std::endl;
  bool all_correct = true;
  const int sample_vertices = std::min(100, static_cast<int>(n));

  for (int i = 0; i < sample_vertices; ++i) {
    // Sample vertices uniformly across the graph
    v_id_t vertex_id = static_cast<v_id_t>(i * n / sample_vertices);

    // Get ground truth from original graph
    std::vector<v_id_t> ground_truth = com_graph.get_neighbors(vertex_id);

    std::vector<v_id_t>& page_result = page_based_all_neighbors[vertex_id];

    // Compare vector with span using std::equal
    bool page_match = (ground_truth.size() == page_result.size()) &&
                      std::equal(ground_truth.begin(), ground_truth.end(), page_result.begin());

    // Report any mismatches found
    if (!page_match) {
      std::cout << "Mismatch type: " << db.vertex_index->items[vertex_id].vertex_type << std::endl;
      std::cout << "Mismatch at vertex " << vertex_id << std::endl;
      std::cout << "  Ground truth size: " << ground_truth.size() << std::endl;
      std::cout << "  Page-based size: " << page_result.size() << std::endl;
      db.read_neighbor_clone(vertex_id); // For debugging
      all_correct = false;
      std::cout << "Page-based neighbors: ";
      for (auto neighbor : page_result) {
        std::cout << neighbor << "->";
      }
      std::cout << std::endl;
      std::cout << "  Ground truth neighbors: ";
      for (auto neighbor : ground_truth) {
        std::cout << neighbor << "->";
      }
      std::cout << std::endl;
      break;
    }
  }

  std::cout << "Sample verification (" << sample_vertices
            << " vertices): " << (all_correct ? "PASSED" : "FAILED") << std::endl;

  auto start_time = std::chrono::high_resolution_clock::now();
  auto bfs_result = db.bfs(0);
  auto end_time = std::chrono::high_resolution_clock::now();

  auto elapsed_microseconds =
      std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
  std::cout << "BFS completed in " << elapsed_microseconds.count() << " microseconds." << std::endl;
  std::cout << "BFS visited " << bfs_result.size() << " vertices." << std::endl;

  return 0;
}