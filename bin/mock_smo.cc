#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"
#include "bw_graph/mem/mem_edges.h"

#include <cassert>
#include <chrono>
#include <iostream>

int main(int argc, char* argv[]) {
  std::string config_path = "config/default.yaml";

  if (argc > 1) {
    config_path = argv[1];
    std::cout << "Config path: " << config_path << std::endl;
    load_yaml_config(config_path);
  } else {
    std::cout << "Use Default Configuration." << std::endl;
    load_yaml_config();
  }

  print_config();

  std::string g_name = bw_graph::GRAPH_NAME;

  std::cout << "Creating database.." << std::endl;
  auto db =
      bw_graph_db_t(g_name, bw_graph::BW_GRAPH_REBUILD_INDEX, bw_graph::BW_GRAPH_REDO_PARTITION,
                    false, bw_graph::BW_GRAPH_ID_AWARE, true, true);
  assert(db.graph_name_ == g_name);
  std::cout << "Database created successfully" << std::endl;
  db.warm_up();

  // Load the delta edges;
  mem_edge_list_t inserted_edges =
      mem_edge_list_t::generate_workload_for_smo(g_name, db.get_vertex_count(), 3);
  std::cout << "Generate delta edges: " << inserted_edges.edge_count() << std::endl;

  // Record start time
  auto start_time = std::chrono::high_resolution_clock::now();

  inserted_edges.apply_parallel([&](v_id_t src, v_id_t dst) { db.insert_edge_direct(src, dst); }, 4,
                                insert_mode_t::SEQUENTIAL);

  // Record end time
  auto end_time = std::chrono::high_resolution_clock::now();

  // Calculate elapsed time in milliseconds
  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  // Calculate throughput (edges per second)
  double elapsed_sec = elapsed_ms / 1000.0;
  double throughput = inserted_edges.edge_count() / elapsed_sec;

  // Print results
  std::cout << "\n=== Performance Statistics ===" << std::endl;
  std::cout << "Total edges processed: " << inserted_edges.edge_count() << std::endl;
  std::cout << "MODE: " << bw_graph::BW_GRAPH_INSERTION_MODE << std::endl;
  std::cout << "Total time: " << elapsed_ms << " ms (" << elapsed_sec << " s)" << std::endl;
  std::cout << "Throughput: " << throughput << " edges/sec" << std::endl;
  std::cout << "Average latency: " << (elapsed_ms * 1000.0 / inserted_edges.edge_count())
            << " μs/edge" << std::endl;
  std::cout << "==============================\n" << std::endl;

  return 0;
}