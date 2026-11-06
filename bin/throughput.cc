#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"
#include "bw_graph/mem/mem_edges.h"

#include <cassert>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
constexpr double kDefaultZipfianAlpha = 0.8;
constexpr size_t kDefaultZipfianMaxEdgesPerSource = 10000;

void print_usage(const char* program) {
  std::cerr << "Usage: " << program
            << " [config_path] [insert_mode] [zipfian_alpha] [zipfian_max_edges_per_source]\n"
            << "  insert_mode: 0=sequential, 1=random, 2=zipfian\n"
            << "  zipfian_alpha and zipfian_max_edges_per_source are used only when mode=2\n"
            << "  zipfian_max_edges_per_source=0 means uncapped\n";
}
} // namespace

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

// Get mode name string
static std::string get_mode_name(int mode) {
  switch (mode) {
  case 0:
    return "sequential";
  case 1:
    return "random";
  case 2:
    return "zipfian";
  default:
    return "unknown";
  }
}

// Generate log filename
static std::string generate_log_filename(const std::string& mode_name,
                                         const std::string& graph_name) {
  std::ostringstream oss;
  oss << "throughput-" << mode_name << "-" << graph_name << ".log";
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
                         const std::string& mode_name, int mode_value, size_t edge_count,
                         double elapsed_ms, double throughput, double avg_latency_us) {
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
  log_file << "Insert Mode: " << mode_name << " (mode=" << mode_value << ")\n";
  log_file << "Total Edges: " << edge_count << "\n";
  log_file << "\n";

  log_file << "--- Performance Statistics ---\n";
  double elapsed_sec = elapsed_ms / 1000.0;
  log_file << "Total Time: " << elapsed_ms << " ms (" << elapsed_sec << " s)\n";
  log_file << "Throughput: " << throughput << " edges/sec\n";
  log_file << "Average Latency: " << avg_latency_us << " μs/edge\n";
  log_file << "========================================\n\n";

  log_file.close();
  std::cout << "Results written to: " << log_path << std::endl;
}

int main(int argc, char* argv[]) {
  std::string config_path = "config/default.yaml";
  int insert_mode_value = -1; // -1 means use config file value
  double zipfian_alpha = kDefaultZipfianAlpha;
  size_t zipfian_max_edges_per_source = kDefaultZipfianMaxEdgesPerSource;

  // Parse command line arguments
  try {
    if (argc > 5) {
      print_usage(argv[0]);
      return 1;
    }

    if (argc > 1) {
      config_path = argv[1];
      std::cout << "Config path: " << config_path << std::endl;
    }

    if (argc > 2) {
      insert_mode_value = std::stoi(argv[2]);
      std::cout << "Insert mode (from command line): " << insert_mode_value << std::endl;
    }

    if (argc > 3) {
      zipfian_alpha = std::stod(argv[3]);
      std::cout << "Zipfian alpha (from command line): " << zipfian_alpha << std::endl;
    }

    if (argc > 4) {
      zipfian_max_edges_per_source = static_cast<size_t>(std::stoull(argv[4]));
      std::cout << "Zipfian max edges per source (from command line): "
                << zipfian_max_edges_per_source << std::endl;
    }
  } catch (const std::exception& e) {
    std::cerr << "Invalid command line argument: " << e.what() << std::endl;
    print_usage(argv[0]);
    return 1;
  }

  if (argc == 2) {
    std::cout << "Use Default Configuration." << std::endl;
  }

  load_yaml_config(config_path);
  print_config();

  std::string g_name = bw_graph::GRAPH_NAME;

  // Ensure log directory exists
  ensure_log_directory();

  std::cout << "Creating database.." << std::endl;
  auto db =
      bw_graph_db_t(g_name, bw_graph::BW_GRAPH_REBUILD_INDEX, bw_graph::BW_GRAPH_REDO_PARTITION,
                    bw_graph::BW_GRAPH_NEIGHBOR_COMPRESS, bw_graph::BW_GRAPH_ID_AWARE, true, true);
  db.warm_up();
  assert(db.graph_name_ == g_name);
  std::cout << "Database created successfully" << std::endl;

  // Load the delta edges;
  mem_edge_list_t inserted_edges(g_name);
  inserted_edges.load_from_file();
  std::cout << "Loading delta edges: " << inserted_edges.edge_count() << std::endl;

  // Check the insert mode;
  // Use command line argument if provided, otherwise use config value
  int final_mode =
      (insert_mode_value != -1) ? insert_mode_value : bw_graph::BW_GRAPH_INSERTION_MODE;

  insert_mode_t insert_mode = insert_mode_t::SEQUENTIAL;
  if (final_mode == 0) {
    // Seq.
    insert_mode = insert_mode_t::DEFAULT;
    std::cout << "Insert mode: SEQUENTIAL" << std::endl;
  } else if (final_mode == 1) {
    // Rand.
    insert_mode = insert_mode_t::RANDOM;
    std::cout << "Insert mode: RANDOM" << std::endl;
  } else if (final_mode == 2) {
    // To zipfian.
    insert_mode = insert_mode_t::DEFAULT;
    inserted_edges.to_zipfian(zipfian_alpha, 0, zipfian_max_edges_per_source);
    std::cout << "Insert mode: ZIPFIAN (skew=" << zipfian_alpha
              << ", max_edges_per_source=" << zipfian_max_edges_per_source << ")" << std::endl;
  } else {
    std::cout << "ERROR: Invalid insert mode: " << final_mode << std::endl;
    return 1;
  }

  std::string mode_name = get_mode_name(final_mode);

  // Record start time
  auto start_time = std::chrono::high_resolution_clock::now();

  inserted_edges.apply_parallel([&](v_id_t src, v_id_t dst) { db.insert_edge(src, dst); }, 4,
                                insert_mode);

  // Record end time
  auto end_time = std::chrono::high_resolution_clock::now();

  // Calculate elapsed time in milliseconds
  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  // Calculate throughput (edges per second)
  double elapsed_sec = elapsed_ms / 1000.0;
  double throughput = inserted_edges.edge_count() / elapsed_sec;
  double avg_latency_us = (elapsed_ms * 1000.0 / inserted_edges.edge_count());

  db.wait_for_pending_consolidations();

  // Print results
  std::cout << "\n=== Performance Statistics ===" << std::endl;
  std::cout << "Total edges processed: " << inserted_edges.edge_count() << std::endl;
  std::cout << "Insert Mode: " << mode_name << " (mode=" << final_mode << ")" << std::endl;
  std::cout << "Total time: " << elapsed_ms << " ms (" << elapsed_sec << " s)" << std::endl;
  std::cout << "Throughput: " << throughput << " edges/sec" << std::endl;
  std::cout << "Average latency: " << avg_latency_us << " μs/edge" << std::endl;
  std::cout << "==============================\n" << std::endl;

  // Generate log filename and write results
  std::string log_filename = generate_log_filename(mode_name, g_name);
  std::cout << "Log file: ./logs/" << log_filename << std::endl;

  write_to_log(log_filename, g_name, mode_name, final_mode, inserted_edges.edge_count(), elapsed_ms,
               throughput, avg_latency_us);

  return 0;
}
