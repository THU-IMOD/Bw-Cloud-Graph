#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"

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
  oss << "static-algorithm-" << graph_name << "-" << generate_timestamp_log() << ".log";
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

// Write header to log file
static void write_log_header(std::ofstream& log_file, const std::string& graph_name) {
  log_file << "Graph Algorithm Performance Test\n";
  log_file << "Experiment Timestamp: " << generate_timestamp_log() << "\n";
  log_file << "Dataset: " << graph_name << "\n\n";
}

// Write BFS results to log file
static void write_bfs_results(std::ofstream& log_file, bool is_random, const std::string& algo,
                              uint64_t rounds, double total_us, double average_us = 0.0,
                              uint64_t start_vertex = 0) {
  log_file << "--- BFS Performance Statistics ---\n";
  log_file << "Algo: " << algo << "\n";
  log_file << "Mode: " << (algo == "sequential" ? "Sequential" : "Parallel") << "\n";

  if (is_random) {
    log_file << "Test Type: Random vertices\n";
    log_file << "Total BFS rounds: " << rounds << "\n";
    log_file << "Total time: " << total_us << " us\n";
    log_file << "Average BFS time: " << average_us << " us\n";
  } else {
    log_file << "Test Type: Single vertex\n";
    log_file << "Start vertex: " << start_vertex << "\n";
    log_file << "BFS execution time: " << total_us << " us\n";
  }
  log_file << "\n";
}

// Write WCC results to log file
static void write_wcc_results(std::ofstream& log_file, const std::string& algo, double total_us) {
  log_file << "--- WCC Performance Statistics ---\n";
  log_file << "Algo: " << algo << "\n";
  log_file << "Mode: " << (algo == "sequential" ? "Sequential" : "Parallel") << "\n";
  log_file << "WCC execution time: " << total_us << " us\n\n";
}

// Write PageRank results to log file
static void write_pagerank_results(std::ofstream& log_file, const std::string& algo,
                                   double total_us, int max_iterations, double damping_factor,
                                   double convergence_threshold) {
  log_file << "--- PageRank Performance Statistics ---\n";
  log_file << "Algo: " << algo << "\n";
  log_file << "Mode: " << (algo == "sequential" ? "Sequential" : "Parallel") << "\n";
  log_file << "Max iterations: " << max_iterations << "\n";
  log_file << "Damping factor: " << damping_factor << "\n";
  log_file << "Convergence threshold: " << convergence_threshold << "\n";
  log_file << "PageRank execution time: " << total_us << " us\n\n";
}

// Write CDLP results to log file
static void write_cdlp_results(std::ofstream& log_file, const std::string& algo, double total_us,
                               int max_iterations) {
  log_file << "--- CDLP Performance Statistics ---\n";
  log_file << "Algo: " << algo << "\n";
  log_file << "Mode: " << (algo == "sequential" ? "Sequential" : "Parallel") << "\n";
  log_file << "Max iterations: " << max_iterations << "\n";
  log_file << "CDLP execution time: " << total_us << " us\n\n";
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

  // Generate log filename and open file
  std::string log_filename = generate_log_filename(g_name);
  std::string log_path = "./logs/" + log_filename;
  std::ofstream log_file(log_path);

  if (!log_file.is_open()) {
    std::cerr << "Failed to open log file: " << log_path << std::endl;
    return 1;
  }

  std::cout << "Log file: " << log_path << std::endl;

  // Write log header
  write_log_header(log_file, g_name);

  std::cout << "Creating database.." << std::endl;
  auto db =
      bw_graph_db_t(g_name, bw_graph::BW_GRAPH_REBUILD_INDEX, bw_graph::BW_GRAPH_REDO_PARTITION,
                    false, bw_graph::BW_GRAPH_ID_AWARE, true, true);
  assert(db.graph_name_ == g_name);
  std::cout << "Database created successfully" << std::endl;

  // Warm up the database
  db.warm_up();

  // Perform BFS
  // Dispatch helper: run one BFS from start_vertex, return elapsed microseconds.
  const std::string& bfs_algo = bw_graph::BW_GRAPH_BFS_ALGO;
  auto run_bfs_once = [&](uint64_t start_vertex) -> double {
    if (bfs_algo == "map") {
      return db.benchmark_parallel_bfs_map(start_vertex) * 1e6;
    } else if (bfs_algo == "scan") {
      return db.benchmark_parallel_bfs_scan(start_vertex) * 1e6;
    } else {
      // sequential (or unknown algo)
      auto t0 = std::chrono::high_resolution_clock::now();
      db.bfs(start_vertex);
      auto t1 = std::chrono::high_resolution_clock::now();
      return static_cast<double>(
          std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    }
  };

  if (bw_graph::BW_GRAPH_BFS_IS_RANDOM) {
    std::cout << "Performing BFS from random vertices (algo=" << bfs_algo << ")" << std::endl;
    const uint64_t bfs_rounds = bw_graph::BW_GRAPH_BFS_RANDOM_SELECT;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, db.get_vertex_count() - 1);

    double total_us = 0.0;
    for (uint64_t i = 0; i < bfs_rounds; ++i)
      total_us += run_bfs_once(dis(gen));
    double average_us = total_us / bfs_rounds;

    std::cout << "\nBFS Performance Test Results (Random)\n";
    std::cout << "Algo: " << bfs_algo << std::endl;
    std::cout << "Total BFS rounds: " << bfs_rounds << std::endl;
    std::cout << "Total time: " << total_us << " us" << std::endl;
    std::cout << "Average BFS time: " << average_us << " us\n" << std::endl;
    write_bfs_results(log_file, true, bfs_algo, bfs_rounds, total_us, average_us);
  } else {
    std::cout << "Performing BFS from a specific vertex (algo=" << bfs_algo << ")" << std::endl;
    uint64_t start_vertex = bw_graph::BW_GRAPH_BFS_START_VERTEX;
    double total_us = run_bfs_once(start_vertex);

    std::cout << "\nBFS Performance Test Results (Single)\n";
    std::cout << "Algo: " << bfs_algo << std::endl;
    std::cout << "Start vertex: " << start_vertex << std::endl;
    std::cout << "BFS execution time: " << total_us << " us\n" << std::endl;
    write_bfs_results(log_file, false, bfs_algo, 1, total_us, 0.0, start_vertex);
  }

  // Perform WCC
  const std::string& wcc_algo = bw_graph::BW_GRAPH_WCC_ALGO;
  std::cout << "Performing WCC algorithm (algo=" << wcc_algo << ")" << std::endl;

  double wcc_total_us;
  if (wcc_algo == "sv") {
    wcc_total_us = db.benchmark_wcc_sv() * 1e6;
  } else if (wcc_algo == "map") {
    wcc_total_us = db.benchmark_wcc_map() * 1e6;
  } else if (wcc_algo == "scan") {
    wcc_total_us = db.benchmark_parallel_wcc_scan() * 1e6;
  } else {
    // sequential (or unknown algo)
    auto t0 = std::chrono::high_resolution_clock::now();
    db.wcc();
    auto t1 = std::chrono::high_resolution_clock::now();
    wcc_total_us =
        static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  }

  std::cout << "\nWCC Performance Test Results\n";
  std::cout << "Algo: " << wcc_algo << std::endl;
  std::cout << "WCC execution time: " << wcc_total_us << " us\n" << std::endl;
  write_wcc_results(log_file, wcc_algo, wcc_total_us);

  // Perform PageRank
  const std::string& pr_algo = bw_graph::BW_GRAPH_PAGERANK_ALGO;
  std::cout << "Performing PageRank algorithm (algo=" << pr_algo << ")" << std::endl;

  double pr_total_us;
  if (pr_algo == "map") {
    pr_total_us =
        db.benchmark_parallel_pagerank_map(bw_graph::BW_GRAPH_PAGERANK_MAX_ITERATION,
                                           bw_graph::BW_GRAPH_PAGERANK_DAMPING_FACTOR,
                                           bw_graph::BW_GRAPH_PAGERANK_CONVERGENCE_THRESHOLD) *
        1e6;
  } else if (pr_algo == "scan") {
    pr_total_us =
        db.benchmark_parallel_pagerank_scan(bw_graph::BW_GRAPH_PAGERANK_MAX_ITERATION,
                                            bw_graph::BW_GRAPH_PAGERANK_DAMPING_FACTOR,
                                            bw_graph::BW_GRAPH_PAGERANK_CONVERGENCE_THRESHOLD) *
        1e6;
  } else {
    // sequential (or unknown algo)
    auto t0 = std::chrono::high_resolution_clock::now();
    db.pagerank(bw_graph::BW_GRAPH_PAGERANK_MAX_ITERATION,
                bw_graph::BW_GRAPH_PAGERANK_DAMPING_FACTOR,
                bw_graph::BW_GRAPH_PAGERANK_CONVERGENCE_THRESHOLD);
    auto t1 = std::chrono::high_resolution_clock::now();
    pr_total_us =
        static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  }

  std::cout << "\nPageRank Performance Test Results\n";
  std::cout << "Algo: " << pr_algo << std::endl;
  std::cout << "PageRank execution time: " << pr_total_us << " us\n" << std::endl;
  write_pagerank_results(log_file, pr_algo, pr_total_us, bw_graph::BW_GRAPH_PAGERANK_MAX_ITERATION,
                         bw_graph::BW_GRAPH_PAGERANK_DAMPING_FACTOR,
                         bw_graph::BW_GRAPH_PAGERANK_CONVERGENCE_THRESHOLD);

  // Perform CDLP
  const std::string& cdlp_algo = bw_graph::BW_GRAPH_CDLP_ALGO;
  const std::string& cdlp_variant = bw_graph::BW_GRAPH_CDLP_VARIANT;
  std::cout << "Performing CDLP algorithm (algo=" << cdlp_algo << ", variant=" << cdlp_variant
            << ")" << std::endl;

  double cdlp_total_us;
  if (cdlp_algo == "map") {
    if (cdlp_variant == "voting") {
      cdlp_total_us = db.benchmark_cdlp_voting_map(bw_graph::BW_GRAPH_CDLP_MAX_ITERATION) * 1e6;
    } else {
      // default: original fixed-slot voting
      cdlp_total_us = db.benchmark_parallel_cdlp_map(bw_graph::BW_GRAPH_CDLP_MAX_ITERATION) * 1e6;
    }
  } else if (cdlp_algo == "scan") {
    cdlp_total_us = db.benchmark_parallel_cdlp_scan(bw_graph::BW_GRAPH_CDLP_MAX_ITERATION) * 1e6;
  } else {
    // sequential (or unknown algo)
    auto t0 = std::chrono::high_resolution_clock::now();
    db.cdlp(bw_graph::BW_GRAPH_CDLP_MAX_ITERATION);
    auto t1 = std::chrono::high_resolution_clock::now();
    cdlp_total_us =
        static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  }

  std::cout << "\nCDLP Performance Test Results\n";
  std::cout << "Algo: " << cdlp_algo << "  Variant: " << cdlp_variant << std::endl;
  std::cout << "CDLP execution time: " << cdlp_total_us << " us\n" << std::endl;
  write_cdlp_results(log_file, cdlp_algo, cdlp_total_us, bw_graph::BW_GRAPH_CDLP_MAX_ITERATION);

  log_file.close();
  std::cout << "Results written to: " << log_path << std::endl;

  return 0;
}
