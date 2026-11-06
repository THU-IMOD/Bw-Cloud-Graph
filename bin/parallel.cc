#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"
#include "bw_graph/mem/mem_g0com.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <oneapi/tbb.h>
#include <thread>
#include <vector>

// Simulate a time-consuming computation task
void heavy_computation(int& value) {
  // Simulate compute-intensive work
  for (int i = 0; i < 1000000; ++i) {
    value += i % 100;
  }
  // Add some delay to make parallel effect more obvious
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// Parallel test;
void parallel_enable_test() {
  const int data_size = 100;
  std::vector<int> data(data_size, 0);
  std::vector<int> data_copy(data_size, 0);

  std::cout << "OneTBB Parallel Execution Demo\n";
  std::cout << "Data Size: " << data_size << " elements\n";
  std::cout << "Hardware Concurrency: " << std::thread::hardware_concurrency() << " threads\n\n";

  // 1. Serial execution
  std::cout << "1. Running serial execution...\n";
  auto start_serial = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < data_size; ++i) {
    heavy_computation(data[i]);
  }

  auto end_serial = std::chrono::high_resolution_clock::now();
  auto serial_time =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_serial - start_serial);

  // 2. Parallel execution (using parallel_for)
  std::cout << "2. Running parallel execution...\n";
  auto start_parallel = std::chrono::high_resolution_clock::now();

  tbb::parallel_for(tbb::blocked_range<int>(0, data_size),
                    [&](const tbb::blocked_range<int>& range) {
                      for (int i = range.begin(); i != range.end(); ++i) {
                        heavy_computation(data_copy[i]);
                      }
                    });

  auto end_parallel = std::chrono::high_resolution_clock::now();
  auto parallel_time =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_parallel - start_parallel);

  // 3. Display results
  std::cout << "\n=== Execution Results ===\n";
  std::cout << "Serial execution time: " << serial_time.count() << " ms\n";
  std::cout << "Parallel execution time: " << parallel_time.count() << " ms\n";

  if (parallel_time.count() > 0) {
    double speedup = static_cast<double>(serial_time.count()) / parallel_time.count();
    std::cout << "Speedup: " << speedup << "x\n";

    if (speedup > 1.5) {
      std::cout << "✓ Program is indeed running in parallel!\n";
    } else {
      std::cout << "? Speedup not significant, possibly due to small task size "
                   "or system load\n";
    }
  }

  // 4. Verify result correctness
  bool results_match = true;
  for (int i = 0; i < data_size; ++i) {
    if (data[i] != data_copy[i]) {
      results_match = false;
      break;
    }
  }

  std::cout << "Result correctness: " << (results_match ? "✓ Passed" : "✗ Failed") << "\n";

  // 5. Display thread usage information
  std::cout << "\n=== TBB Information ===\n";
  std::cout << "TBB default thread count: " << tbb::this_task_arena::max_concurrency() << "\n";
}

void load_page_parallel() {
  std::string config_path = "config/sd.yaml";

  std::cout << "Use Default Configuration." << std::endl;
  load_yaml_config(config_path);
  print_config();

  std::string g_name = bw_graph::GRAPH_NAME;

  // Load input graph file
  std::string test_file = "data/" + g_name + ".graph";
  mem_sub_com_t com_graph(test_file, true);

  std::cout << "Ground Truth Graph loaded: " << com_graph.vertex_count() << " vertices, "
            << com_graph.edge_count() << " edges" << std::endl;

  std::cout << "Creating database.." << std::endl;
  auto db = bw_graph_db_t(g_name);

  // Create a disk manager
  const std::filesystem::path test_db_file = bw_graph::csr_db_path(g_name);
  std::unique_ptr<disk_manager_t> disk_manager = std::make_unique<disk_manager_t>(test_db_file);
  uint64_t start_page = 0;
  uint64_t end_page = 1000;

  std::cout << "Hardware concurrency: " << std::thread::hardware_concurrency() << " threads\n";
  std::cout << "TBB max concurrency: " << tbb::this_task_arena::max_concurrency() << " threads\n\n";

  //=====================
  //  Serial Loading
  //=====================
  std::cout << "Loading pages serially from " << start_page << " to " << (end_page - 1) << "...\n";

  auto start_time = std::chrono::high_resolution_clock::now();

  std::vector<std::vector<char>> pages;
  pages.reserve(end_page - start_page);

  for (page_no_t page_no = start_page; page_no < end_page; ++page_no) {
    std::vector<char> page_data(bw_graph::BW_GRAPH_PAGE_SIZE);
    disk_manager->read_page(page_no, page_data.data());
    pages.push_back(std::move(page_data));
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  auto serial_duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  std::cout << "Serial loading completed in " << serial_duration.count() << " ms\n\n";

  //=====================
  //  Parallel Loading
  //=====================
  std::cout << "Loading pages in parallel from " << start_page << " to " << (end_page - 1)
            << "...\n";

  start_time = std::chrono::high_resolution_clock::now();

  std::vector<std::vector<char>> pages_in_parallel(end_page - start_page);

  // Initialize all page vectors with correct size
  for (auto& page : pages_in_parallel) {
    page.resize(bw_graph::BW_GRAPH_PAGE_SIZE);
  }

  // Thread-safe progress tracking
  std::atomic<int> pages_loaded{0};
  std::mutex progress_mutex;

  // Use parallel_for to load pages concurrently
  tbb::parallel_for(tbb::blocked_range<page_no_t>(start_page, end_page),
                    [&](const tbb::blocked_range<page_no_t>& range) {
                      for (page_no_t page_no = range.begin(); page_no != range.end(); ++page_no) {
                        size_t index = page_no - start_page;
                        disk_manager->read_page(page_no, pages_in_parallel[index].data());

                        // Thread-safe progress reporting
                        int current_loaded = pages_loaded.fetch_add(1) + 1;
                        if (current_loaded % 200 == 0) {
                          std::lock_guard<std::mutex> lock(progress_mutex);
                          std::cout << "Progress: " << current_loaded << "/"
                                    << (end_page - start_page) << " pages loaded\n";
                        }
                      }
                    });

  end_time = std::chrono::high_resolution_clock::now();
  auto parallel_duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  std::cout << "Parallel loading completed in " << parallel_duration.count() << " ms\n\n";

  //=====================
  //  Data Validation
  //=====================
  std::cout << "=== Data Validation ===\n";
  bool data_matches = true;
  size_t mismatch_count = 0;

  for (size_t i = 0; i < pages.size(); ++i) {
    if (pages[i].size() != pages_in_parallel[i].size()) {
      data_matches = false;
      mismatch_count++;
      continue;
    }

    if (std::memcmp(pages[i].data(), pages_in_parallel[i].data(), pages[i].size()) != 0) {
      data_matches = false;
      mismatch_count++;
      if (mismatch_count <= 5) { // Only show first 5 mismatches
        std::cout << "Mismatch found in page " << (start_page + i) << "\n";
      }
    }
  }

  std::cout << "Data validation result: " << (data_matches ? "✓ PASSED" : "✗ FAILED") << "\n";
  if (!data_matches) {
    std::cout << "Total mismatches: " << mismatch_count << " out of " << pages.size() << " pages\n";
  }

  //=====================
  //  Performance Summary
  //=====================
  std::cout << "\n=== Performance Summary ===\n";
  std::cout << "Serial execution time: " << serial_duration.count() << " ms\n";
  std::cout << "Parallel execution time: " << parallel_duration.count() << " ms\n";

  if (parallel_duration.count() > 0) {
    double speedup = static_cast<double>(serial_duration.count()) / parallel_duration.count();
    std::cout << "Speedup: " << std::fixed << std::setprecision(2) << speedup << "x\n";

    double efficiency = speedup / tbb::this_task_arena::max_concurrency();
    std::cout << "Parallel efficiency: " << std::fixed << std::setprecision(2) << (efficiency * 100)
              << "%\n";
  }

  // Clean up
  disk_manager.reset();

  std::cout << "\n=== Final Result ===\n";
  if (data_matches) {
    std::cout << "✓ Parallel page loading is working correctly!\n";
    std::cout << "✓ All data integrity checks passed.\n";
  } else {
    std::cout << "✗ Data integrity issues detected.\n";
    std::cout << "  Please check disk manager thread safety.\n";
  }

  // Test Parallel BFS
  start_time = std::chrono::high_resolution_clock::now();
  com_graph.bfs(0);
  end_time = std::chrono::high_resolution_clock::now();
  serial_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  start_time = std::chrono::high_resolution_clock::now();
  com_graph.bfs_parallel(0);
  end_time = std::chrono::high_resolution_clock::now();
  parallel_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  std::cout << "\nBFS Performance:\n";
  std::cout << "Serial execution time: " << serial_duration.count() << " ms\n";
  std::cout << "Parallel execution time: " << parallel_duration.count() << " ms\n";

  if (parallel_duration.count() > 0) {
    double speedup = static_cast<double>(serial_duration.count()) / parallel_duration.count();
    std::cout << "Speedup: " << std::fixed << std::setprecision(2) << speedup << "x\n";

    double efficiency = speedup / tbb::this_task_arena::max_concurrency();
    std::cout << "Parallel efficiency: " << std::fixed << std::setprecision(2) << (efficiency * 100)
              << "%\n";
  }
}

void block_bfs(std::string config_path = "config/rn.yaml") {
  std::cout << "Use Default Configuration." << std::endl;
  load_yaml_config(config_path);
  print_config();

  std::string g_name = bw_graph::GRAPH_NAME;

  // Load input graph file
  std::string test_file = "data/" + g_name + ".graph";
  mem_sub_com_t com_graph(test_file, true);

  std::cout << "Ground Truth Graph loaded: " << com_graph.vertex_count() << " vertices, "
            << com_graph.edge_count() << " edges" << std::endl;

  std::cout << "Creating database.." << std::endl;
  auto db =
      bw_graph_db_t(g_name, bw_graph::BW_GRAPH_REBUILD_INDEX, bw_graph::BW_GRAPH_REDO_PARTITION);
  std::cout << "Buffer Pool Warming Up..." << std::endl;
  db.warm_up();
  std::cout << "Start BFS" << std::endl;
  auto start_time = std::chrono::high_resolution_clock::now();
  auto bfs_result = db.bfs(1);
  auto end_time = std::chrono::high_resolution_clock::now();

  auto elapsed_microseconds =
      std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
  std::cout << "BFS completed in " << elapsed_microseconds.count() << " microseconds." << std::endl;
  auto res = com_graph.bfs(1);
  std::cout << "Visited " << bfs_result.size() << " vertices." << std::endl;
  std::cout << "Ground truth visited " << res.size() << " vertices." << std::endl;
}

// Test read pages in parallel
void parallel_read_page() {
  std::string config_path = "config/rn.yaml";

  std::cout << "Use Default Configuration." << std::endl;
  load_yaml_config(config_path);
  print_config();

  std::string g_name = bw_graph::GRAPH_NAME;

  // Load input graph file
  std::string test_file = "data/" + g_name + ".graph";
  mem_sub_com_t com_graph(test_file, true);

  std::cout << "Ground Truth Graph loaded: " << com_graph.vertex_count() << " vertices, "
            << com_graph.edge_count() << " edges" << std::endl;

  std::cout << "Creating database.." << std::endl;
  auto db = bw_graph_db_t(g_name);
  const uint64_t n = com_graph.vertex_count();
  std::cout << "Buffer Pool Warming Up..." << std::endl;
  for (v_id_t vertex_id = 0; vertex_id < n; ++vertex_id) {
    auto neighbors = db.read_neighbor_clone(vertex_id);
  }
  std::cout << "Perform Concurrent Read Neighbors;" << std::endl;

  auto start_time = std::chrono::high_resolution_clock::now();
  // ==========================
  //  Parallel Read Neighbors
  // ==========================

  const int num_threads = 10;
  const int reads_per_thread = 10000;

  // Statistics
  std::atomic<int> total_reads{0};
  std::atomic<int> total_neighbors_found{0};
  std::atomic<int> read_failures{0};

  std::cout << "\n=== Starting Concurrent Read Neighbors Test ===" << std::endl;
  std::cout << "Configuration:" << std::endl;
  std::cout << "- Threads: " << num_threads << std::endl;
  std::cout << "- Reads per thread: " << reads_per_thread << std::endl;
  std::cout << "- Total reads: " << (num_threads * reads_per_thread) << std::endl;
  std::cout << "- Vertex range: [0, " << (n - 1) << "]" << std::endl;
  std::cout << "Starting concurrent read test..." << std::endl;

  auto read_neighbors_task = [&](const tbb::blocked_range<int>& range) {
    // Random number generator for each thread
    std::random_device rd;
    std::mt19937 gen(rd() + range.begin());
    std::uniform_int_distribution<v_id_t> vertex_dis(0, n - 1);

    for (int thread_id = range.begin(); thread_id != range.end(); ++thread_id) {
      int local_reads = 0;
      int local_neighbors_count = 0;
      int local_failures = 0;

      for (int read_op = 0; read_op < reads_per_thread; ++read_op) {
        try {
          // Generate random vertex id
          v_id_t vertex_id = vertex_dis(gen);

          // Read neighbors
          auto neighbors = db.read_neighbor_clone(vertex_id);

          local_reads++;
          local_neighbors_count += neighbors.size();

          // Progress report every 1000 reads
          if (read_op % 1000 == 0 && read_op > 0) {
            std::cout << "Thread " << thread_id << " completed " << read_op
                      << " reads (Total neighbors: " << local_neighbors_count << ")" << std::endl;
          }

        } catch (const std::exception& e) {
          local_failures++;
          std::cout << "Thread " << thread_id << " read failed: " << e.what() << std::endl;
        } catch (...) {
          local_failures++;
          std::cout << "Thread " << thread_id << " read failed with unknown exception" << std::endl;
        }
      }

      // Update global counters
      total_reads += local_reads;
      total_neighbors_found += local_neighbors_count;
      read_failures += local_failures;

      std::cout << "Thread " << thread_id << " completed all reads - "
                << "Successful reads: " << local_reads
                << ", Total neighbors found: " << local_neighbors_count
                << ", Failures: " << local_failures << std::endl;
    }
  };

  // Execute parallel read operations
  tbb::parallel_for(tbb::blocked_range<int>(0, num_threads), read_neighbors_task);

  // ==========================
  auto end_time = std::chrono::high_resolution_clock::now();

  auto elapsed_microseconds =
      std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

  // Print the testing results
  std::cout << "\n=== Concurrent Read Neighbors Test Results ===" << std::endl;
  std::cout << "Test Configuration:" << std::endl;
  std::cout << "  Total threads: " << num_threads << std::endl;
  std::cout << "  Reads per thread: " << reads_per_thread << std::endl;
  std::cout << "  Vertex count: " << n << std::endl;

  std::cout << "\nRead Statistics:" << std::endl;
  std::cout << "  Total successful reads: " << total_reads.load() << std::endl;
  std::cout << "  Total neighbors found: " << total_neighbors_found.load() << std::endl;
  std::cout << "  Read failures: " << read_failures.load() << std::endl;

  if (total_reads.load() > 0) {
    double avg_neighbors = (double) total_neighbors_found.load() / total_reads.load();
    std::cout << "  Average neighbors per vertex: " << std::fixed << std::setprecision(2)
              << avg_neighbors << std::endl;

    double success_rate =
        ((double) (total_reads.load()) * 100.0) / (num_threads * reads_per_thread);
    std::cout << "  Read success rate: " << std::fixed << std::setprecision(2) << success_rate
              << "%" << std::endl;
  }

  std::cout << "\nPerformance Metrics:" << std::endl;
  std::cout << "  Test duration: " << elapsed_microseconds.count() << " microseconds ("
            << (elapsed_microseconds.count() / 1000.0) << " ms)" << std::endl;

  if (elapsed_microseconds.count() > 0) {
    double reads_per_second = (total_reads.load() * 1000000.0) / elapsed_microseconds.count();
    double neighbors_per_second =
        (total_neighbors_found.load() * 1000000.0) / elapsed_microseconds.count();

    std::cout << "  Reads per second: " << std::fixed << std::setprecision(2) << reads_per_second
              << std::endl;
    std::cout << "  Neighbors processed per second: " << std::fixed << std::setprecision(2)
              << neighbors_per_second << std::endl;

    double avg_read_time = (double) elapsed_microseconds.count() / total_reads.load();
    std::cout << "  Average read time: " << std::fixed << std::setprecision(2) << avg_read_time
              << " microseconds" << std::endl;
  }

  std::cout << "\nConcurrency Analysis:" << std::endl;
  int total_expected_reads = num_threads * reads_per_thread;
  double efficiency = (total_reads.load() * 100.0) / total_expected_reads;
  std::cout << "  Expected reads: " << total_expected_reads << std::endl;
  std::cout << "  Actual successful reads: " << total_reads.load() << std::endl;
  std::cout << "  Execution efficiency: " << std::fixed << std::setprecision(2) << efficiency << "%"
            << std::endl;

  std::cout << "\n=== Concurrent Read Test Completed Successfully ===" << std::endl;
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
  block_bfs(config_path);
  return 0;
}
