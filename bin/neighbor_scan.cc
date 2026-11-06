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
#include <vector>

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
  oss << "neighbor-scan-" << graph_name << "-" << generate_timestamp_log() << ".log";
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
                         uint64_t vertex_count, uint64_t sample_count, double total_us,
                         double average_latency_us) {
  std::string log_path = "./logs/" + filename;
  std::ofstream log_file(log_path);

  if (!log_file.is_open()) {
    std::cerr << "Failed to open log file: " << log_path << std::endl;
    return;
  }

  log_file << "Neighbor Scan Performance Test\n";
  log_file << "Experiment Timestamp: " << generate_timestamp_log() << "\n";
  log_file << "Dataset: " << graph_name << "\n\n";

  log_file << "--- Graph Statistics ---\n";
  log_file << "Total vertex count: " << vertex_count << "\n";
  log_file << "Sample count (10%): " << sample_count << "\n\n";

  log_file << "--- Performance Statistics ---\n";
  log_file << "Total vertices tested: " << sample_count << "\n";
  log_file << "Total time: " << total_us << " us\n";
  log_file << "Average neighbor scan latency: " << average_latency_us << " us\n";

  // Calculate throughput (scans per second)
  double total_seconds = total_us / 1000000.0;
  double throughput = sample_count / total_seconds;
  log_file << "Throughput: " << throughput << " scans/sec\n";

  log_file.close();
  std::cout << "Results written to: " << log_path << std::endl;
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

  // Ensure log directory exists
  ensure_log_directory();

  std::cout << "Creating database.." << std::endl;
  auto db =
      bw_graph_db_t(g_name, bw_graph::BW_GRAPH_REBUILD_INDEX, bw_graph::BW_GRAPH_REDO_PARTITION,
                    false, bw_graph::BW_GRAPH_ID_AWARE, true, true);
  assert(db.graph_name_ == g_name);
  std::cout << "Database created successfully" << std::endl;

  // Warm up the database
  db.warm_up();

  // Get total vertex count
  const uint64_t vertex_count = db.get_vertex_count();
  std::cout << "Total vertex count: " << vertex_count << std::endl;

  // Calculate 10% of vertices for sampling
  const uint64_t sample_count = vertex_count / 10;
  std::cout << "Sample count (10%): " << sample_count << std::endl;

  // Generate random vertex IDs (from 1 to vertex_count - 1)
  std::vector<v_id_t> sampled_vertices;
  sampled_vertices.reserve(sample_count);

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<v_id_t> dist(1, vertex_count - 1);

  for (uint64_t i = 0; i < sample_count; ++i) {
    sampled_vertices.push_back(dist(gen));
  }

  std::cout << "Starting neighbor scan performance test..." << std::endl;

  // Record start time
  auto start_time = std::chrono::high_resolution_clock::now();

  // Execute read_neighbor for each sampled vertex
  // Execute read_neighbor for each sampled vertex in parallel
  tbb::parallel_for(tbb::blocked_range<size_t>(0, sampled_vertices.size()),
                    [&](const tbb::blocked_range<size_t>& range) {
                      for (size_t i = range.begin(); i != range.end(); ++i) {
                        auto neighbors = db.read_neighbor(sampled_vertices[i]);
                        (void)neighbors;
                      }
                    });

  // Record end time
  auto end_time = std::chrono::high_resolution_clock::now();

  // Calculate total elapsed time in microseconds
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
  double total_us = duration.count();

  // Calculate average latency per neighbor scan
  double average_latency_us = total_us / sample_count;

  // Calculate throughput
  double total_seconds = total_us / 1000000.0;
  double throughput = sample_count / total_seconds;

  // Print results to console
  std::cout << "\nNeighbor Scan Performance Test Results\n";
  std::cout << "Total vertices tested: " << sample_count << std::endl;
  std::cout << "Total time: " << total_us << " us" << std::endl;
  std::cout << "Average neighbor scan latency: " << average_latency_us << " us" << std::endl;
  std::cout << "Throughput: " << throughput << " scans/sec\n" << std::endl;

  // Generate log filename and write results
  std::string log_filename = generate_log_filename(g_name);
  write_to_log(log_filename, g_name, vertex_count, sample_count, total_us, average_latency_us);

  return 0;
}