#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <unordered_set>
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
  oss << "vertex-delete-" << graph_name << ".log";
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
                         v_id_t initial_vertex_count, uint32_t num_vertices_deleted,
                         uint64_t actual_deleted_count, int num_threads, double elapsed_ms,
                         double throughput) {
  std::string log_path = "./logs/" + filename;
  std::ofstream log_file(log_path, std::ios::app);

  if (!log_file.is_open()) {
    std::cerr << "Failed to open log file: " << log_path << std::endl;
    return;
  }

  double elapsed_sec = elapsed_ms / 1000.0;
  double avg_latency_us = elapsed_ms * 1000.0 / actual_deleted_count;
  v_id_t remaining_vertices = initial_vertex_count - actual_deleted_count;

  log_file << "========================================\n";
  log_file << "Experiment Timestamp: " << generate_timestamp_log() << "\n";
  log_file << "========================================\n";
  log_file << "Dataset: " << graph_name << "\n";
  log_file << "Initial vertex count: " << initial_vertex_count << "\n";
  log_file << "Vertices to delete: " << num_vertices_deleted << "\n";
  log_file << "Vertices deleted: " << actual_deleted_count << "\n";
  log_file << "Remaining vertices: " << remaining_vertices << "\n";
  log_file << "Number of threads: " << num_threads << "\n";
  log_file << "\n";

  log_file << "--- Time Statistics ---\n";
  log_file << "Total time: " << elapsed_ms << " ms (" << elapsed_sec << " s)\n";
  log_file << "\n";

  log_file << "--- Throughput ---\n";
  log_file << "Throughput: " << throughput << " vertices/sec\n";
  log_file << "\n";

  log_file << "--- Latency ---\n";
  log_file << "Average latency: " << avg_latency_us << " μs/vertex\n";
  log_file << "========================================\n\n";

  log_file.close();
  std::cout << "Results written to: " << log_path << std::endl;
}

int main(int argc, char* argv[]) {
  std::string config_path = "config/default.yaml";
  uint32_t NUM_VERTICES_TO_DELETE = 100000;
  const int NUM_THREADS = 12;

  // Parse command line arguments
  if (argc > 1) {
    config_path = argv[1];
    std::cout << "Config path: " << config_path << std::endl;
  }

  if (argc > 2) {
    NUM_VERTICES_TO_DELETE = std::atoi(argv[2]);
    std::cout << "Vertices to delete: " << NUM_VERTICES_TO_DELETE << std::endl;
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
  assert(db.graph_name_ == g_name);
  std::cout << "Database created successfully" << std::endl;
  db.warm_up();

  // Get the current vertex count
  v_id_t vertex_count = db.get_vertex_count();
  std::cout << "Current vertex count: " << vertex_count << std::endl;
  std::cout << "Current max vertex ID: " << (vertex_count - 1) << std::endl;

  // Check if we have enough vertices to delete
  if (vertex_count < NUM_VERTICES_TO_DELETE) {
    std::cerr << "Error: Not enough vertices in the database. "
              << "Required: " << NUM_VERTICES_TO_DELETE << ", Available: " << vertex_count
              << std::endl;
    return 1;
  }

  std::cout << "\n=== Vertex Deletion Configuration ===" << std::endl;
  std::cout << "Vertices to delete: " << NUM_VERTICES_TO_DELETE << std::endl;
  std::cout << "Number of threads: " << NUM_THREADS << std::endl;
  std::cout << "======================================\n" << std::endl;

  // Initialize random number generator
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<v_id_t> dist(0, vertex_count - 1);

  // Generate random unique vertex IDs to delete
  std::unordered_set<v_id_t> vertices_to_delete_set;
  std::cout << "Selecting random vertices to delete..." << std::endl;

  while (vertices_to_delete_set.size() < NUM_VERTICES_TO_DELETE) {
    v_id_t random_vertex = dist(gen);
    vertices_to_delete_set.insert(random_vertex);
  }

  // Convert to vector for easier iteration
  std::vector<v_id_t> vertices_to_delete(vertices_to_delete_set.begin(),
                                         vertices_to_delete_set.end());

  std::cout << "Selected " << vertices_to_delete.size() << " unique vertices for deletion"
            << std::endl;

  // Atomic counter for progress tracking
  std::atomic<uint64_t> deleted_count{0};

  // Create TBB task arena with specified number of threads
  tbb::task_arena arena(NUM_THREADS);

  // Record start time
  auto start_time = std::chrono::high_resolution_clock::now();

  // Parallel deletion using TBB
  arena.execute([&] {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, vertices_to_delete.size()),
                      [&](const tbb::blocked_range<size_t>& range) {
                        for (size_t i = range.begin(); i < range.end(); ++i) {
                          // Delete vertex
                          db.delete_vertex(vertices_to_delete[i], true);

                          // Update counter
                          uint64_t count = deleted_count.fetch_add(1, std::memory_order_relaxed);

                          // Progress reporting (every 10000 vertices)
                          if ((count + 1) % 10000 == 0) {
                            std::cout << "Progress: " << (count + 1) << " / "
                                      << NUM_VERTICES_TO_DELETE << " vertices deleted" << std::endl;
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
  double throughput = deleted_count.load() / elapsed_sec;

  // Print results
  std::cout << "\n=== Vertex Deletion Performance Statistics ===" << std::endl;
  std::cout << "Total vertices deleted: " << deleted_count.load() << std::endl;
  std::cout << "Remaining vertices: " << (vertex_count - deleted_count.load()) << std::endl;
  std::cout << "\n--- Time Statistics ---" << std::endl;
  std::cout << "Total time: " << elapsed_ms << " ms (" << elapsed_sec << " s)" << std::endl;
  std::cout << "\n--- Throughput ---" << std::endl;
  std::cout << "Throughput: " << throughput << " vertices/sec" << std::endl;
  std::cout << "\n--- Latency ---" << std::endl;
  std::cout << "Average latency: " << (elapsed_ms * 1000.0 / deleted_count.load()) << " μs/vertex"
            << std::endl;
  std::cout << "================================================\n" << std::endl;

  // Write results to log file
  write_to_log(log_filename, g_name, vertex_count, NUM_VERTICES_TO_DELETE, deleted_count.load(),
               NUM_THREADS, elapsed_ms, throughput);

  return 0;
}