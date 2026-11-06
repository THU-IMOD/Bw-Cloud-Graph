#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"
#include "bw_graph/mem/mem_edges.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
#include <thread>

// Statistics for each workload
struct WorkloadStats {
  uint64_t operation_count;
  double elapsed_seconds;
  double throughput;
  double avg_latency_us;
};

std::atomic<bool> analytical_finished{false};
std::atomic<bool> transactional_finished{false};

WorkloadStats analytical_stats;
WorkloadStats transactional_stats;

// Analytical thread: continuous neighbor scan
void analytical_workload(bw_graph_db_t& db, uint64_t vertex_count, uint64_t target_duration_ms) {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<v_id_t> dist(1, vertex_count - 1);

  std::cout << "[Analytical] Starting neighbor scan workload..." << std::endl;

  auto start_time = std::chrono::high_resolution_clock::now();
  uint64_t scan_count = 0;

  // Run until target duration is reached
  while (true) {
    // Random vertex for neighbor scan
    v_id_t vertex_id = dist(gen);
    db.read_neighbor(vertex_id);
    scan_count++;

    // Check if duration exceeded
    auto current_time = std::chrono::high_resolution_clock::now();
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time).count();

    if (static_cast<uint64_t>(elapsed_ms) >= target_duration_ms) {
      break;
    }
  }

  auto end_time = std::chrono::high_resolution_clock::now();

  // Calculate statistics
  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
  double elapsed_sec = elapsed_ms / 1000.0;

  analytical_stats.operation_count = scan_count;
  analytical_stats.elapsed_seconds = elapsed_sec;
  analytical_stats.throughput = scan_count / elapsed_sec;
  analytical_stats.avg_latency_us = (elapsed_ms * 1000.0) / scan_count;

  analytical_finished.store(true);

  std::cout << "[Analytical] Completed " << scan_count << " neighbor scans in " << elapsed_ms
            << " ms" << std::endl;
}

// Transactional thread: edge insertion
void transactional_workload(bw_graph_db_t& db, mem_edge_list_t& edges) {
  std::cout << "[Transactional] Starting edge insertion workload..." << std::endl;

  auto start_time = std::chrono::high_resolution_clock::now();

  uint64_t insertion_count = 0;

  // Insert all edges as fast as possible
  edges.apply_sequential([&](v_id_t src, v_id_t dst) {
    db.insert_edge(src, dst);
    insertion_count++;
  });

  auto end_time = std::chrono::high_resolution_clock::now();

  // Calculate statistics
  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
  double elapsed_sec = elapsed_ms / 1000.0;

  transactional_stats.operation_count = insertion_count;
  transactional_stats.elapsed_seconds = elapsed_sec;
  transactional_stats.throughput = insertion_count / elapsed_sec;
  transactional_stats.avg_latency_us = (elapsed_ms * 1000.0) / insertion_count;

  transactional_finished.store(true);

  std::cout << "[Transactional] Completed " << insertion_count << " edge insertions in "
            << elapsed_ms << " ms" << std::endl;
}

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

  // Warm up the database
  db.warm_up();

  // Get vertex count for analytical workload
  const uint64_t vertex_count = db.get_vertex_count();
  std::cout << "Total vertex count: " << vertex_count << std::endl;

  // Load delta edges for transactional workload
  mem_edge_list_t inserted_edges(g_name);
  inserted_edges.load_from_file();
  std::cout << "Loading delta edges: " << inserted_edges.edge_count() << std::endl;

  // Estimate how long insertion will take based on expected throughput
  // Assume ~1-4 million edges/sec insertion rate
  double estimated_insertion_time_sec =
      inserted_edges.edge_count() / 2000000.0; // Conservative estimate
  uint64_t target_duration_ms = static_cast<uint64_t>(estimated_insertion_time_sec * 1000);

  std::cout << "Estimated insertion time: " << estimated_insertion_time_sec << " seconds"
            << std::endl;
  std::cout << "Analytical workload will run for approximately " << target_duration_ms << " ms"
            << std::endl;

  std::cout << "\n========================================" << std::endl;
  std::cout << "Starting HTAP Workload Test" << std::endl;
  std::cout << "========================================\n" << std::endl;

  // Launch both threads simultaneously
  std::thread analytical_thread(analytical_workload, std::ref(db), vertex_count,
                                target_duration_ms);

  std::thread transactional_thread(transactional_workload, std::ref(db), std::ref(inserted_edges));

  // Wait for both threads to complete independently
  transactional_thread.join();
  analytical_thread.join();

  // Print results
  std::cout << "\n========================================" << std::endl;
  std::cout << "HTAP Workload Test Results" << std::endl;
  std::cout << "========================================" << std::endl;

  std::cout << "\n--- Analytical Workload (Neighbor Scan) ---" << std::endl;
  std::cout << "Total neighbor scans: " << analytical_stats.operation_count << std::endl;
  std::cout << "Execution time: " << (analytical_stats.elapsed_seconds * 1000) << " ms ("
            << analytical_stats.elapsed_seconds << " s)" << std::endl;
  std::cout << "Scan throughput: " << analytical_stats.throughput << " scans/sec" << std::endl;
  std::cout << "Average scan latency: " << analytical_stats.avg_latency_us << " μs/scan"
            << std::endl;

  std::cout << "\n--- Transactional Workload (Edge Insertion) ---" << std::endl;
  std::cout << "Total edge insertions: " << transactional_stats.operation_count << std::endl;
  std::cout << "Execution time: " << (transactional_stats.elapsed_seconds * 1000) << " ms ("
            << transactional_stats.elapsed_seconds << " s)" << std::endl;
  std::cout << "Insertion throughput: " << transactional_stats.throughput << " edges/sec"
            << std::endl;
  std::cout << "Average insertion latency: " << transactional_stats.avg_latency_us << " μs/edge"
            << std::endl;

  std::cout << "\n--- Workload Overlap Analysis ---" << std::endl;
  double time_diff =
      std::abs(analytical_stats.elapsed_seconds - transactional_stats.elapsed_seconds);
  double overlap_percentage =
      (std::min(analytical_stats.elapsed_seconds, transactional_stats.elapsed_seconds) /
       std::max(analytical_stats.elapsed_seconds, transactional_stats.elapsed_seconds)) *
      100.0;
  std::cout << "Time difference: " << (time_diff * 1000) << " ms" << std::endl;
  std::cout << "Overlap percentage: " << overlap_percentage << "%" << std::endl;

  std::cout << "========================================\n" << std::endl;

  return 0;
}