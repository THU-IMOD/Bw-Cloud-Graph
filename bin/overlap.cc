#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"
#include "bw_graph/mem/mem_edges.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <indicators/progress_bar.hpp>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <tbb/task_group.h>
#include <unordered_set>
#include <vector>

// ============ Experiment Stats ============
struct experiment_stats_t {
  double target_pct = 0.0;
  double actual_pct = 0.0;
  uint64_t overlap_hits = 0;
  uint64_t scans = 0;
  double scan_ms = 0.0;
  double scan_avg_us = 0.0;
  double scan_throughput = 0.0;
  uint64_t inserted_edges = 0;
  double insert_ms = 0.0;
  double insert_throughput = 0.0;
  double insert_avg_us = 0.0;
  double total_ms = 0.0;
};

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
static std::string generate_log_filename(const std::string& graph_name, double overlap_rate) {
  std::ostringstream oss;
  oss << "overlap-" << graph_name << "-" << static_cast<int>(overlap_rate * 100) << "-"
      << generate_timestamp_log() << ".log";
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
                         double overlap_rate, const experiment_stats_t& stats) {
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
  log_file << "Overlap rate: " << (overlap_rate * 100.0) << "%\n";
  log_file << "\n";
  log_file << std::fixed << std::setprecision(2);
  log_file << "Total time: " << stats.total_ms << " ms\n";
  log_file << "\n";
  log_file << "Insert performance:\n";
  log_file << "  Edges inserted: " << stats.inserted_edges << "\n";
  log_file << "  Throughput: " << (stats.insert_throughput / 1e6) << " M edges/s\n";
  log_file << "  Average latency: " << stats.insert_avg_us << " μs/edge\n";
  log_file << "\n";
  log_file << "Scan performance:\n";
  log_file << "  Total scans: " << stats.scans << "\n";
  log_file << "  Throughput: " << (stats.scan_throughput / 1000.0) << " K scans/s\n";
  log_file << "  Average latency: " << stats.scan_avg_us << " μs/scan\n";
  log_file << "\n";
  log_file << "Overlap statistics:\n";
  log_file << "  Target overlap: " << stats.target_pct << "%\n";
  log_file << "  Actual overlap: " << stats.actual_pct << "%\n";
  log_file << "  Overlap hits: " << stats.overlap_hits << "/" << stats.scans << "\n";
  log_file << "========================================\n\n";

  log_file.close();
  std::cout << "Results written to: " << log_path << std::endl;
}

// Generate Random Edges
static void generate_random_edges(mem_edge_list_t& edge_list, v_id_t vertex_count,
                                  size_t edge_count, int seed = 42) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<v_id_t> dist(0, vertex_count - 1);

  std::cout << "Generating " << edge_count << " random edges..." << std::endl;

  for (size_t i = 0; i < edge_count; ++i) {
    v_id_t src = dist(rng);
    v_id_t dst = dist(rng);

    // Avoid self-loops
    while (src == dst) {
      dst = dist(rng);
    }

    edge_list.add_edge(src, dst);

    if ((i + 1) % 1000000 == 0) {
      std::cout << "  Generated " << (i + 1) << " edges..." << std::endl;
    }
  }

  std::cout << "Generated " << edge_list.edge_count() << " edges" << std::endl;
}

// Generate Scan Queries
static std::vector<v_id_t> generate_scan_queries(const std::vector<v_id_t>& overlap_nodes,
                                                 const std::vector<v_id_t>& non_overlap_nodes,
                                                 double overlap_rate, size_t scan_count,
                                                 int seed = 42) {
  if (scan_count == 0)
    throw std::runtime_error("scan_count cannot be 0");

  std::vector<v_id_t> queries;
  queries.reserve(scan_count);

  size_t overlap_need = static_cast<size_t>(std::round(scan_count * overlap_rate));
  overlap_need = (overlap_need < scan_count) ? overlap_need : scan_count;
  size_t disjoint_need = scan_count - overlap_need;

  if (overlap_nodes.empty() && overlap_need > 0)
    throw std::runtime_error("No overlap nodes available");
  if (non_overlap_nodes.empty() && disjoint_need > 0)
    throw std::runtime_error("No non-overlap nodes available");

  // Add overlap nodes
  for (size_t i = 0; i < overlap_need; ++i)
    queries.push_back(overlap_nodes[i % overlap_nodes.size()]);

  // Add non-overlap nodes
  for (size_t i = 0; i < disjoint_need; ++i)
    queries.push_back(non_overlap_nodes[i % non_overlap_nodes.size()]);

  // Shuffle to randomize access pattern
  std::mt19937_64 rng(seed);
  std::shuffle(queries.begin(), queries.end(), rng);

  return queries;
}

// Concurrent Read-Write Experiment
static experiment_stats_t run_overlap_experiment(bw_graph_db_t& db, mem_edge_list_t& delta_edges,
                                                 const std::vector<char>& overlap_mask,
                                                 const std::vector<v_id_t>& scan_queries,
                                                 double overlap_rate) {
  experiment_stats_t stats;
  stats.target_pct = overlap_rate * 100.0;

  const size_t total_read = scan_queries.size();

  std::atomic<size_t> read_pos{0};
  std::atomic<uint64_t> inserted_edges{0};
  std::atomic<uint64_t> overlap_hits{0};
  std::atomic<bool> insertion_done{false};

  auto start_time = std::chrono::high_resolution_clock::now();

  // Use TBB to launch scan thread
  tbb::task_group tg;

  // Launch scanning task
  tg.run([&]() {
    while (true) {
      size_t idx = read_pos.fetch_add(1, std::memory_order_relaxed);
      if (idx >= total_read) {
        break;
      }

      v_id_t vid = scan_queries[idx];

      // Perform neighbor scan
      auto out_neighbors = db.read_neighbor(vid);
      // Touch the data to ensure it's actually read
      volatile size_t count = out_neighbors.size();
      (void) count;

      if (vid < overlap_mask.size() && overlap_mask[vid]) {
        overlap_hits.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  // Use apply_parallel for edge insertion
  delta_edges.apply_parallel(
      [&](v_id_t src, v_id_t dst) {
        db.insert_edge(src, dst);
        inserted_edges.fetch_add(1, std::memory_order_relaxed);
      },
      4, insert_mode_t::SEQUENTIAL);
  insertion_done.store(true, std::memory_order_release);

  // Wait for all tasks to complete
  tg.wait();

  auto end_time = std::chrono::high_resolution_clock::now();

  double total_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

  // Calculate statistics
  stats.total_ms = total_ms;
  stats.inserted_edges = inserted_edges.load(std::memory_order_relaxed);
  stats.insert_ms = total_ms;
  stats.insert_throughput =
      stats.insert_ms > 0.0 ? (stats.inserted_edges / (stats.insert_ms / 1000.0)) : 0.0;
  stats.insert_avg_us =
      stats.inserted_edges ? (1000.0 * stats.insert_ms / stats.inserted_edges) : 0.0;

  stats.overlap_hits = overlap_hits.load(std::memory_order_relaxed);
  stats.scans = read_pos.load(std::memory_order_relaxed);
  stats.scans = (stats.scans < total_read) ? stats.scans : total_read;
  stats.scan_ms = total_ms;
  stats.scan_avg_us = stats.scans ? (stats.scan_ms * 1000.0 / stats.scans) : 0.0;
  stats.scan_throughput = stats.scan_ms > 0.0 ? (stats.scans / (stats.scan_ms / 1000.0)) : 0.0;
  stats.actual_pct = stats.scans ? (100.0 * stats.overlap_hits / stats.scans) : 0.0;

  return stats;
}

// ============ Print Usage ============
static void print_usage(const char* prog) {
  std::cout << "Usage:\n  " << prog << " <config-file> <overlap-rate> [edge-count]\n";
  std::cout << "\nArguments:\n"
            << "  config-file       Path to configuration file\n"
            << "  overlap-rate      Overlap rate as decimal (e.g., 0.2 for "
               "20%)\n"
            << "  edge-count        (Optional) Number of edges to generate\n"
            << "\nBehavior:\n"
            << "  Without edge-count (2 args):\n"
            << "    - Load edges from file\n"
            << "    - If file has < 10M edges: use file edges\n"
            << "    - If file has >= 10M edges: clear and generate 10M random "
               "edges\n"
            << "  With edge-count (3 args):\n"
            << "    - Force clear file edges\n"
            << "    - Generate specified number of random edges\n";
}

// ============ Main ============
int main(int argc, char* argv[]) {
  if (argc != 3 && argc != 4) {
    print_usage(argv[0]);
    return 1;
  }

  std::string config_path = argv[1];
  double overlap_rate = std::stod(argv[2]);

  // Check if edge count is provided
  bool force_generate = (argc == 4);
  size_t forced_edge_count = 0;

  if (force_generate) {
    forced_edge_count = std::stoull(argv[3]);
    if (forced_edge_count == 0) {
      std::cerr << "Error: edge-count must be greater than 0\n";
      return 1;
    }
  }

  if (overlap_rate < 0.0 || overlap_rate >= 1.0) {
    std::cerr << "Error: overlap-rate must be in [0.0, 1.0)\n";
    return 1;
  }

  std::cout << "Config path: " << config_path << std::endl;
  load_yaml_config(config_path);
  print_config();

  std::string g_name = bw_graph::GRAPH_NAME;

  // Ensure log directory exists
  ensure_log_directory();

  // Generate log filename
  std::string log_filename = generate_log_filename(g_name, overlap_rate);
  std::cout << "Log file: ./logs/" << log_filename << std::endl;

  std::cout << "Creating database..." << std::endl;
  auto db =
      bw_graph_db_t(g_name, bw_graph::BW_GRAPH_REBUILD_INDEX, bw_graph::BW_GRAPH_REDO_PARTITION,
                    false, bw_graph::BW_GRAPH_ID_AWARE, true, true);
  assert(db.graph_name_ == g_name);
  std::cout << "Creating database...[Ok]" << std::endl;

  v_id_t vertex_count = db.get_vertex_count();
  std::cout << "Graph vertex count: " << vertex_count << std::endl;

  db.warm_up();

  // Load edges from file first
  mem_edge_list_t inserted_edges(g_name);

  if (force_generate) {
    // Third parameter provided: force generate edges
    std::cout << "Force generating " << forced_edge_count << " random edges..." << std::endl;
    std::cout << "Loading edges from file..." << std::endl;
    inserted_edges.load_from_file();
    std::cout << "Loaded " << inserted_edges.edge_count() << " edges from file" << std::endl;

    // Clear loaded edges
    std::cout << "Clearing file edges and generating new edges..." << std::endl;
    inserted_edges.clear();

    // Generate specified number of edges
    generate_random_edges(inserted_edges, vertex_count, forced_edge_count, 42);
    std::cout << "Edge Generating...[Ok]" << std::endl;
  } else {
    // No third parameter: use automatic logic
    std::cout << "Loading edges from file..." << std::endl;
    inserted_edges.load_from_file();
    std::cout << "Loaded " << inserted_edges.edge_count() << " edges from file" << std::endl;

    const size_t threshold = 10000000; // 10 million

    // Check if we need to regenerate edges
    if (inserted_edges.edge_count() >= threshold) {
      std::cout << "File contains >= 10M edges, clearing and generating 10M "
                   "random edges..."
                << std::endl;

      // Clear the loaded edges
      inserted_edges.clear();

      // Generate 10M new edges
      generate_random_edges(inserted_edges, vertex_count, threshold, 42);
      std::cout << "Edge Generating...[Ok]" << std::endl;
    } else {
      std::cout << "Using " << inserted_edges.edge_count() << " edges from file" << std::endl;
    }
  }

  // Extract source vertices to build overlap set
  std::unordered_set<v_id_t> source_set;
  for (const auto& edge : inserted_edges.get_edges()) {
    source_set.insert(edge.first);
  }
  std::vector<v_id_t> source_vertices(source_set.begin(), source_set.end());

  // Build overlap mask
  std::vector<char> overlap_mask(vertex_count, 0);
  for (v_id_t v : source_vertices) {
    if (v < vertex_count)
      overlap_mask[v] = 1;
  }

  // Build non-overlap node set
  std::vector<v_id_t> non_overlap_nodes;
  non_overlap_nodes.reserve(vertex_count - source_vertices.size());
  for (v_id_t v = 0; v < vertex_count; ++v) {
    if (!overlap_mask[v])
      non_overlap_nodes.push_back(v);
  }

  std::cout << "\n=== Experiment Configuration ===" << std::endl;
  std::cout << "Dataset: " << g_name << std::endl;
  std::cout << "Overlap rate: " << (overlap_rate * 100.0) << "%" << std::endl;
  std::cout << "Delta edges: " << inserted_edges.edge_count() << std::endl;
  std::cout << "Overlap nodes (sources): " << source_vertices.size() << std::endl;
  std::cout << "Non-overlap nodes: " << non_overlap_nodes.size() << std::endl;
  std::cout << "Worker threads: 4" << std::endl;

  // Generate scan queries (same count as delta edges)
  size_t scan_count = inserted_edges.edge_count();
  std::cout << "Generating " << scan_count << " scan queries..." << std::endl;
  auto scan_queries =
      generate_scan_queries(source_vertices, non_overlap_nodes, overlap_rate, scan_count, 42);
  std::cout << "Scan queries generated...[Ok]" << std::endl;

  std::cout << "Running Overlap Experiment..." << std::endl;
  // Run experiment with 4 threads (sequential insertion mode)
  auto stats = run_overlap_experiment(db, inserted_edges, overlap_mask, scan_queries, overlap_rate);
  std::cout << "Running Overlap Experiment...[Ok]" << std::endl;

  std::cout << "Final Results\n";
  std::cout << std::fixed << std::setprecision(2) << "Total time: " << stats.total_ms << " ms\n"
            << "\nInsert performance:\n"
            << "  Edges inserted: " << stats.inserted_edges << "\n"
            << "  Throughput: " << (stats.insert_throughput / 1e6) << " M edges/s\n"
            << "  Average latency: " << stats.insert_avg_us << " μs/edge\n"
            << "\nScan performance:\n"
            << "  Total scans: " << stats.scans << "\n"
            << "  Throughput: " << (stats.scan_throughput / 1000.0) << " K scans/s\n"
            << "  Average latency: " << stats.scan_avg_us << " μs/scan\n"
            << "\nOverlap statistics:\n"
            << "  Target overlap: " << stats.target_pct << "%\n"
            << "  Actual overlap: " << stats.actual_pct << "%\n"
            << "  Overlap hits: " << stats.overlap_hits << "/" << stats.scans << "\n";

  // Write results to log file
  write_to_log(log_filename, g_name, overlap_rate, stats);

  return 0;
}