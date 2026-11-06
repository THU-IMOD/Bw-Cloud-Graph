#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"
#include "bw_graph/mem/mem_edges.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

// Algorithm execution statistics
struct algorithm_status_t {
  std::string name;
  double execution_time_us;
  bool completed;
};

std::atomic<bool> insertion_running{true};
std::atomic<bool> should_stop_insertion{false}; // New flag to signal stop

// Generate timestamp string
static std::string generate_timestamp_log() {
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  std::tm tm_now;
  localtime_r(&time_t_now, &tm_now);

  std::ostringstream oss;
  oss << std::put_time(&tm_now, "%Y-%m-%d-%H-%M-%S");
  return oss.str();
}

// Generate log filename
static std::string generate_log_filename(const std::string& graph_name) {
  std::ostringstream oss;
  oss << "htap-algo-" << graph_name << "-" << generate_timestamp_log() << ".log";
  return oss.str();
}

// Ensure log directory exists
static void ensure_log_directory() {
  std::filesystem::path log_dir("./logs");
  if (!std::filesystem::exists(log_dir)) {
    std::filesystem::create_directories(log_dir);
    std::cout << "Created log directory: ./logs" << std::endl;
  }
}

// Write results to log file
static void write_to_log(const std::string& filename, const std::string& graph_name,
                         size_t insertion_rate, size_t total_edges, size_t inserted_edges,
                         const std::string& snapshot_name,
                         const std::vector<algorithm_status_t>& results) {
  std::string log_path = "./logs/" + filename;
  std::ofstream log_file(log_path, std::ios::app);

  if (!log_file.is_open()) {
    std::cerr << "Failed to open log file: " << log_path << std::endl;
    return;
  }

  log_file << "========================================\n";
  log_file << "Experiment Timestamp: " << generate_timestamp_log() << "\n";
  log_file << "========================================\n";
  log_file << "Dataset: " << graph_name << "\n";
  log_file << "Snapshot: " << snapshot_name << "\n";
  log_file << "Insertion Rate: " << insertion_rate << " edges/sec\n";
  log_file << "Total Edges: " << total_edges << "\n";
  log_file << "Inserted Edges: " << inserted_edges << " (" << (100.0 * inserted_edges / total_edges)
           << "%)\n";
  log_file << "Insertion Status: " << (insertion_running.load() ? "Running" : "Completed") << "\n";
  log_file << "\n";

  log_file << "--- Algorithm Execution Times ---\n";
  double total_algorithm_time_us = 0;
  for (const auto& stat : results) {
    if (stat.completed) {
      double time_ms = stat.execution_time_us / 1000.0;
      double time_s = stat.execution_time_us / 1000000.0;
      log_file << stat.name << ": " << stat.execution_time_us << " us (" << time_ms << " ms / "
               << time_s << " s)\n";
      total_algorithm_time_us += stat.execution_time_us;
    } else {
      log_file << stat.name << ": Not completed\n";
    }
  }

  double total_algorithm_time_s = total_algorithm_time_us / 1000000.0;
  log_file << "\n";
  log_file << "Total Algorithm Execution Time: " << total_algorithm_time_us << " us ("
           << total_algorithm_time_s << " s)\n";
  log_file << "========================================\n\n";

  log_file.close();
  std::cout << "Results written to: " << log_path << std::endl;
}

// Transactional thread: edge insertion at constant rate
size_t transactional_workload(bw_graph_db_t& db, mem_edge_list_t& edges, size_t edges_per_second) {
  std::cout << "[Transactional] Starting edge insertion at " << edges_per_second << " edges/sec..."
            << std::endl;

  if (edges_per_second == 0 || edges.edge_count() == 0) {
    insertion_running.store(false);
    return 0;
  }

  // Calculate time interval between edges in nanoseconds
  uint64_t interval_ns = 1000000000ULL / edges_per_second;

  const auto& edge_list = edges.get_edges();
  auto start_time = std::chrono::high_resolution_clock::now();

  size_t inserted_count = 0;

  for (size_t i = 0; i < edge_list.size(); ++i) {
    // Check if we should stop
    if (should_stop_insertion.load()) {
      std::cout << "[Transactional] Received stop signal, stopping insertion..." << std::endl;
      break;
    }

    auto edge_start = std::chrono::high_resolution_clock::now();

    // Insert the edge
    const auto& edge = edge_list[i];
    db.insert_edge(edge.first, edge.second);
    inserted_count++;

    // Progress reporting
    if ((i + 1) % edges_per_second == 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::high_resolution_clock::now() - start_time)
                         .count();
      std::cout << "[Transactional] Inserted " << (i + 1) << "/" << edge_list.size() << " edges ("
                << ((i + 1) * 100.0 / edge_list.size()) << "%) - "
                << "Elapsed: " << elapsed << "s" << std::endl;
    }

    // Calculate how long to sleep to maintain target rate
    auto edge_end = std::chrono::high_resolution_clock::now();
    auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(edge_end - edge_start).count();

    // Sleep for remaining time if we processed too fast
    if (elapsed_ns < static_cast<int64_t>(interval_ns)) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(interval_ns - elapsed_ns));
    }
  }

  insertion_running.store(false);

  auto end_time = std::chrono::high_resolution_clock::now();
  auto total_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  std::cout << "[Transactional] Stopped after inserting " << inserted_count << "/"
            << edge_list.size() << " edges in " << total_ms << " ms" << std::endl;

  return inserted_count;
}

// Execute BFS algorithm
algorithm_status_t execute_bfs(bw_graph_db_t& db, int num_threads = 0) {
  algorithm_status_t stats;
  stats.name = "BFS";
  stats.completed = false;

  const std::string& bfs_algo = bw_graph::BW_GRAPH_BFS_ALGO;
  std::cout << "[BFS] Starting BFS algorithm (algo=" << bfs_algo << ")..." << std::endl;

  // Helper: run BFS once from start_vertex, return microseconds.
  auto run_bfs_once = [&](uint64_t start_vertex) -> double {
    if (bfs_algo == "map")
      return db.benchmark_parallel_bfs_map(start_vertex) * 1e6;
    if (bfs_algo == "scan")
      return db.benchmark_parallel_bfs_scan(start_vertex, num_threads) * 1e6;
    // sequential (or unknown algo)
    auto t0 = std::chrono::high_resolution_clock::now();
    db.bfs(start_vertex);
    auto t1 = std::chrono::high_resolution_clock::now();
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  };

  if (bw_graph::BW_GRAPH_BFS_IS_RANDOM) {
    const uint64_t bfs_rounds = bw_graph::BW_GRAPH_BFS_RANDOM_SELECT;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, db.get_vertex_count() - 1);
    double total_us = 0.0;
    for (uint64_t i = 0; i < bfs_rounds; ++i)
      total_us += run_bfs_once(dis(gen));
    stats.execution_time_us = total_us;
  } else {
    stats.execution_time_us = run_bfs_once(bw_graph::BW_GRAPH_BFS_START_VERTEX);
  }

  stats.completed = true;
  std::cout << "[BFS] Completed in " << stats.execution_time_us << " us" << std::endl;
  return stats;
}

// Execute WCC algorithm
algorithm_status_t execute_wcc(bw_graph_db_t& db, int num_threads = 0) {
  algorithm_status_t stats;
  stats.name = "WCC";
  stats.completed = false;

  const std::string& wcc_algo = bw_graph::BW_GRAPH_WCC_ALGO;
  std::cout << "[WCC] Starting WCC algorithm (algo=" << wcc_algo << ")..." << std::endl;

  if (wcc_algo == "sv") {
    stats.execution_time_us = db.benchmark_wcc_sv() * 1e6;
  } else if (wcc_algo == "map") {
    stats.execution_time_us = db.benchmark_wcc_map() * 1e6;
  } else if (wcc_algo == "scan") {
    stats.execution_time_us = db.benchmark_parallel_wcc_scan(num_threads) * 1e6;
  } else {
    // sequential (or unknown algo)
    auto t0 = std::chrono::high_resolution_clock::now();
    db.wcc();
    auto t1 = std::chrono::high_resolution_clock::now();
    stats.execution_time_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  }

  stats.completed = true;
  std::cout << "[WCC] Completed in " << stats.execution_time_us << " us" << std::endl;
  return stats;
}

// Execute PageRank algorithm
algorithm_status_t execute_pagerank(bw_graph_db_t& db, int num_threads = 0) {
  algorithm_status_t stats;
  stats.name = "PageRank";
  stats.completed = false;

  const std::string& pr_algo = bw_graph::BW_GRAPH_PAGERANK_ALGO;
  std::cout << "[PageRank] Starting PageRank algorithm (algo=" << pr_algo << ")..." << std::endl;

  if (pr_algo == "map") {
    stats.execution_time_us =
        db.benchmark_parallel_pagerank_map(bw_graph::BW_GRAPH_PAGERANK_MAX_ITERATION,
                                           bw_graph::BW_GRAPH_PAGERANK_DAMPING_FACTOR,
                                           bw_graph::BW_GRAPH_PAGERANK_CONVERGENCE_THRESHOLD) *
        1e6;
  } else if (pr_algo == "scan") {
    stats.execution_time_us =
        db.benchmark_parallel_pagerank_scan(
            bw_graph::BW_GRAPH_PAGERANK_MAX_ITERATION, bw_graph::BW_GRAPH_PAGERANK_DAMPING_FACTOR,
            bw_graph::BW_GRAPH_PAGERANK_CONVERGENCE_THRESHOLD, num_threads) *
        1e6;
  } else {
    // sequential (or unknown algo)
    auto t0 = std::chrono::high_resolution_clock::now();
    db.pagerank(bw_graph::BW_GRAPH_PAGERANK_MAX_ITERATION,
                bw_graph::BW_GRAPH_PAGERANK_DAMPING_FACTOR,
                bw_graph::BW_GRAPH_PAGERANK_CONVERGENCE_THRESHOLD);
    auto t1 = std::chrono::high_resolution_clock::now();
    stats.execution_time_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  }

  stats.completed = true;
  std::cout << "[PageRank] Completed in " << stats.execution_time_us << " us" << std::endl;
  return stats;
}

// Execute CDLP algorithm
algorithm_status_t execute_cdlp(bw_graph_db_t& db, int num_threads = 0) {
  algorithm_status_t stats;
  stats.name = "CDLP";
  stats.completed = false;

  const std::string& cdlp_algo = bw_graph::BW_GRAPH_CDLP_ALGO;
  const std::string& cdlp_variant = bw_graph::BW_GRAPH_CDLP_VARIANT;
  std::cout << "[CDLP] Starting CDLP algorithm (algo=" << cdlp_algo << ", variant=" << cdlp_variant
            << ")..." << std::endl;

  if (cdlp_algo == "map") {
    if (cdlp_variant == "voting") {
      stats.execution_time_us =
          db.benchmark_cdlp_voting_map(bw_graph::BW_GRAPH_CDLP_MAX_ITERATION) * 1e6;
    } else {
      // default: original fixed-slot voting
      stats.execution_time_us =
          db.benchmark_parallel_cdlp_map(bw_graph::BW_GRAPH_CDLP_MAX_ITERATION) * 1e6;
    }
  } else if (cdlp_algo == "scan") {
    stats.execution_time_us =
        db.benchmark_parallel_cdlp_scan(bw_graph::BW_GRAPH_CDLP_MAX_ITERATION, num_threads) * 1e6;
  } else {
    // sequential (or unknown algo)
    auto t0 = std::chrono::high_resolution_clock::now();
    db.cdlp(bw_graph::BW_GRAPH_CDLP_MAX_ITERATION);
    auto t1 = std::chrono::high_resolution_clock::now();
    stats.execution_time_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  }

  stats.completed = true;
  std::cout << "[CDLP] Completed in " << stats.execution_time_us << " us" << std::endl;
  return stats;
}

static std::vector<algorithm_status_t> execute_algorithm_suite(bw_graph_db_t& db, int num_threads,
                                                               const std::string& snapshot_name) {
  std::cout << "\n========================================" << std::endl;
  std::cout << "Running algorithms on " << snapshot_name << std::endl;
  std::cout << "========================================\n" << std::endl;

  std::vector<algorithm_status_t> results;
  results.reserve(4);
  results.push_back(execute_bfs(db, num_threads));
  results.push_back(execute_wcc(db, num_threads));
  results.push_back(execute_pagerank(db, num_threads));
  results.push_back(execute_cdlp(db, num_threads));
  return results;
}

static void print_algorithm_results(const std::string& snapshot_name,
                                    const std::vector<algorithm_status_t>& results) {
  std::cout << "\n--- " << snapshot_name << " Algorithm Execution Times ---" << std::endl;

  double total_algorithm_time_us = 0;
  for (const auto& stat : results) {
    if (!stat.completed) {
      std::cout << stat.name << ": Not completed" << std::endl;
      continue;
    }

    double time_ms = stat.execution_time_us / 1000.0;
    double time_s = stat.execution_time_us / 1000000.0;
    std::cout << stat.name << ": " << stat.execution_time_us << " us (" << time_ms << " ms / "
              << time_s << " s)" << std::endl;
    total_algorithm_time_us += stat.execution_time_us;
  }

  std::cout << "Total Algorithm Execution Time: " << total_algorithm_time_us << " us ("
            << total_algorithm_time_us / 1000000.0 << " s)" << std::endl;
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

  // CLI overrides take precedence over YAML (argv[2..] parsed as --key=value).
  apply_cli_overrides(argc, argv);

  print_config();

  std::string g_name = bw_graph::GRAPH_NAME;

  // Ensure log directory exists
  ensure_log_directory();

  // Generate log filename
  std::string log_filename = generate_log_filename(g_name);
  std::cout << "Log file: ./logs/" << log_filename << std::endl;

  std::cout << "Creating database.." << std::endl;
  auto db =
      bw_graph_db_t(g_name, bw_graph::BW_GRAPH_REBUILD_INDEX, bw_graph::BW_GRAPH_REDO_PARTITION,
                    false, bw_graph::BW_GRAPH_ID_AWARE, true, true, true);
  assert(db.graph_name_ == g_name);
  std::cout << "Database created successfully" << std::endl;

  // Warm up the database
  db.warm_up();

  // Load delta edges for transactional workload
  mem_edge_list_t inserted_edges(g_name);
  inserted_edges.load_from_file();
  std::cout << "Loading delta edges: " << inserted_edges.edge_count() << std::endl;

  // Set insertion rate (edges per second)
  size_t insertion_rate = bw_graph::BW_GRAPH_EDGE_INSERT_RATE;
  std::cout << "Edge insertion rate: " << insertion_rate << " edges/sec" << std::endl;

  double estimated_insertion_time =
      static_cast<double>(inserted_edges.edge_count()) / insertion_rate;
  std::cout << "Estimated insertion time: " << estimated_insertion_time << " seconds" << std::endl;

  // Compute thread allocation: 8 threads reserved for edge insertion,
  // the remainder are used for graph algorithms.
  const int insert_threads = 1;
  const int total_threads = static_cast<int>(std::thread::hardware_concurrency());
  const int algo_threads = std::max(1, total_threads - insert_threads);
  std::cout << "Total hardware threads: " << total_threads << std::endl;
  std::cout << "Insertion threads reserved: " << insert_threads << std::endl;
  std::cout << "Algorithm threads: " << algo_threads << std::endl;

  std::cout << "\n========================================" << std::endl;
  std::cout << "Starting HTAP Algorithm Workload Test" << std::endl;
  std::cout << "========================================\n" << std::endl;

  // Variable to store actual inserted edge count
  size_t actual_inserted_edges = 0;

  // Start edge insertion thread
  std::thread insertion_thread([&]() {
    actual_inserted_edges = transactional_workload(db, inserted_edges, insertion_rate);
  });

  // Wait a bit for insertion to start
  std::this_thread::sleep_for(std::chrono::seconds(1));

  // Keep the original vertex-index snapshot stable while all four algorithms
  // run concurrently with edge insertion.
  std::vector<algorithm_status_t> before_switch_results =
      execute_algorithm_suite(db, algo_threads, "snapshot before version switch");

  // Signal insertion thread to stop
  std::cout << "\n[Main] All algorithms completed, signaling insertion to "
               "stop..."
            << std::endl;
  should_stop_insertion.store(true);

  // Wait for insertion thread to complete
  insertion_thread.join();

  std::cout << "[Main] Edge insertion stopped. Publishing all staged SMO versions..." << std::endl;
  db.force_switch_new_version();
  std::cout << "[Main] Version switch completed." << std::endl;

  // Print final results
  std::cout << "\n========================================" << std::endl;
  std::cout << "HTAP Algorithm Execution Results" << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << "Insertion Rate: " << insertion_rate << " edges/sec" << std::endl;
  std::cout << "Total Edges: " << inserted_edges.edge_count() << std::endl;
  std::cout << "Inserted Edges: " << actual_inserted_edges << " ("
            << (100.0 * actual_inserted_edges / inserted_edges.edge_count()) << "%)" << std::endl;
  std::cout << "Insertion Status: " << (insertion_running.load() ? "Running" : "Stopped")
            << std::endl;

  print_algorithm_results("Snapshot before version switch", before_switch_results);

  std::cout << "========================================\n" << std::endl;

  // Write results to log file
  write_to_log(log_filename, g_name, insertion_rate, inserted_edges.edge_count(),
               actual_inserted_edges, "Snapshot before version switch", before_switch_results);

  return 0;
}
