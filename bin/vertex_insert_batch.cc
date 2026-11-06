#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <folly/container/F14Map.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

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
  oss << "vertex-insert-" << graph_name << ".log";
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
                         int num_vertices_inserted, int neighbors_per_vertex, int num_threads,
                         size_t total_edges, double elapsed_ms, double vertex_throughput,
                         double edge_throughput) {
  std::string log_path = "./logs/" + filename;
  std::ofstream log_file(log_path, std::ios::app);

  if (!log_file.is_open()) {
    std::cerr << "Failed to open log file: " << log_path << std::endl;
    return;
  }

  double elapsed_sec = elapsed_ms / 1000.0;
  double avg_latency_per_vertex_us = elapsed_ms * 1000.0 / num_vertices_inserted;
  double avg_latency_per_edge_us = elapsed_ms * 1000.0 / total_edges;

  log_file << "========================================\n";
  log_file << "Experiment Timestamp: " << generate_timestamp_log() << "\n";
  log_file << "========================================\n";
  log_file << "Dataset: " << graph_name << "\n";
  log_file << "Vertices inserted: " << num_vertices_inserted << "\n";
  log_file << "Neighbors per vertex: " << neighbors_per_vertex << "\n";
  log_file << "Number of threads: " << num_threads << "\n";
  log_file << "Total edges: " << total_edges << "\n";
  log_file << "\n";

  log_file << "--- Time Statistics ---\n";
  log_file << "Total time: " << elapsed_ms << " ms (" << elapsed_sec << " s)\n";
  log_file << "\n";

  log_file << "--- Throughput ---\n";
  log_file << "Vertex throughput: " << vertex_throughput << " vertices/sec\n";
  log_file << "Edge throughput: " << edge_throughput << " edges/sec\n";
  log_file << "\n";

  log_file << "--- Latency ---\n";
  log_file << "Average latency per vertex: " << avg_latency_per_vertex_us << " μs/vertex\n";
  log_file << "Average latency per edge: " << avg_latency_per_edge_us << " μs/edge\n";
  log_file << "========================================\n\n";

  log_file.close();
  std::cout << "Results written to: " << log_path << std::endl;
}

int main(int argc, char* argv[]) {
  std::string config_path = "config/default.yaml";
  int NUM_VERTICES_TO_INSERT = 1000000;
  int NUM_NEIGHBORS_PER_VERTEX = 10;
  const int NUM_THREADS = 12;

  // Parse command line arguments
  if (argc > 1) {
    config_path = argv[1];
    std::cout << "Config path: " << config_path << std::endl;
  }

  if (argc > 2) {
    NUM_VERTICES_TO_INSERT = std::atoi(argv[2]);
    std::cout << "Vertices to insert: " << NUM_VERTICES_TO_INSERT << std::endl;
  }

  if (argc > 3) {
    NUM_NEIGHBORS_PER_VERTEX = std::atoi(argv[3]);
    std::cout << "Neighbors per vertex: " << NUM_NEIGHBORS_PER_VERTEX << std::endl;
  }

  if (argc == 1) {
    std::cout << "Use Default Configuration." << std::endl;
  }

  load_yaml_config(config_path);
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
                    false, bw_graph::BW_GRAPH_ID_AWARE, true, true);
  db.warm_up();
  assert(db.graph_name_ == g_name);
  std::cout << "Database created successfully" << std::endl;

  // Get the current vertex count
  v_id_t vertex_count = db.get_vertex_count();
  std::cout << "Current vertex count: " << vertex_count << std::endl;
  std::cout << "Current max vertex ID: " << (vertex_count - 1) << std::endl;

  std::cout << "\n=== Batch Insertion Configuration ===" << std::endl;
  std::cout << "Vertices to insert: " << NUM_VERTICES_TO_INSERT << std::endl;
  std::cout << "Neighbors per vertex: " << NUM_NEIGHBORS_PER_VERTEX << std::endl;
  std::cout << "Number of threads: " << NUM_THREADS << std::endl;
  std::cout << "Starting vertex ID: " << vertex_count << std::endl;
  std::cout << "======================================\n" << std::endl;

  // Initialize random number generator
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<v_id_t> dist(0, vertex_count - 1);

  // Prepare neighbor lists for all vertices
  std::cout << "Preparing vertex batch with random neighbors..." << std::endl;

  std::vector<std::vector<v_id_t>> all_neighbors(NUM_VERTICES_TO_INSERT);
  for (int i = 0; i < NUM_VERTICES_TO_INSERT; i++) {
    std::vector<v_id_t> neighbors;
    for (int j = 0; j < NUM_NEIGHBORS_PER_VERTEX; j++) {
      v_id_t neighbor = dist(gen);
      neighbors.push_back(neighbor);
    }
    all_neighbors[i] = std::move(neighbors);
  }

  std::cout << "Batch prepared." << std::endl;
  size_t total_edges = NUM_VERTICES_TO_INSERT * NUM_NEIGHBORS_PER_VERTEX;
  std::cout << "Total edges to insert: " << total_edges << std::endl;

  // Atomic counter for progress tracking
  std::atomic<uint64_t> vertices_inserted{0};

  // Create TBB task arena with specified number of threads
  tbb::task_arena arena(NUM_THREADS);

  // Record start time
  auto start_time = std::chrono::high_resolution_clock::now();

  // Parallel insertion using TBB
  arena.execute([&] {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, NUM_VERTICES_TO_INSERT),
                      [&](const tbb::blocked_range<size_t>& range) {
                        for (size_t i = range.begin(); i < range.end(); ++i) {
                          // Insert vertex with its neighbors

                          db.insert_vertex_with_neighbors(all_neighbors[i]);

                          // Update counter
                          uint64_t count =
                              vertices_inserted.fetch_add(1, std::memory_order_relaxed);

                          // Progress reporting (every 10000 vertices)
                          if ((count + 1) % 100000 == 0) {
                            std::cout << "Progress: " << (count + 1) << " / "
                                      << NUM_VERTICES_TO_INSERT << " vertices inserted"
                                      << std::endl;
                          }
                        }
                      });
  });

  // Record end time
  auto end_time = std::chrono::high_resolution_clock::now();

  // Calculate elapsed time
  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  double elapsed_sec = elapsed_ms / 1000.0;

  // Calculate throughput
  double vertex_throughput = NUM_VERTICES_TO_INSERT / elapsed_sec;
  double edge_throughput = total_edges / elapsed_sec;

  // Print results
  std::cout << "\n=== Batch Vertex Insertion Performance Statistics ===" << std::endl;
  std::cout << "Total vertices inserted: " << vertices_inserted.load() << std::endl;
  std::cout << "Total edges inserted: " << total_edges << std::endl;
  std::cout << "\n--- Time Statistics ---" << std::endl;
  std::cout << "Total time: " << elapsed_ms << " ms (" << elapsed_sec << " s)" << std::endl;
  std::cout << "\n--- Throughput ---" << std::endl;
  std::cout << "Vertex throughput: " << vertex_throughput << " vertices/sec" << std::endl;
  std::cout << "Edge throughput: " << edge_throughput << " edges/sec" << std::endl;
  std::cout << "\n--- Latency ---" << std::endl;
  std::cout << "Average latency per vertex: " << (elapsed_ms * 1000.0 / NUM_VERTICES_TO_INSERT)
            << " μs/vertex" << std::endl;
  std::cout << "Average latency per edge: " << (elapsed_ms * 1000.0 / total_edges) << " μs/edge"
            << std::endl;
  std::cout << "======================================================\n" << std::endl;

  // Write results to log file
  write_to_log(log_filename, g_name, NUM_VERTICES_TO_INSERT, NUM_NEIGHBORS_PER_VERTEX, NUM_THREADS,
               total_edges, elapsed_ms, vertex_throughput, edge_throughput);

  return 0;
}