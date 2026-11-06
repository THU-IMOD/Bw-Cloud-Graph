#include "bw_graph/db/db.h"
#include "bw_graph/mem/mem_g0com.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

int main(int argc, char* argv[]) {
  std::string config_path = "config/default.yaml";

  if (argc > 1) {
    config_path = argv[1];
    std::cout << "Config path: " << config_path << std::endl;
    load_yaml_config(config_path);
  } else {
    std::cout << "Use Defalult Configuration." << std::endl;
    load_yaml_config();
  }

  print_config();

  std::string g_name = bw_graph::GRAPH_NAME;

  // Load input graph file
  std::string test_file = "data/" + g_name + ".graph";
  mem_sub_com_t com_graph(test_file, true);

  std::cout << "Creating database.." << std::endl;
  auto db = bw_graph_db_t(g_name);
  assert(db.graph_name_ == g_name);
  std::cout << "Database created successfully" << std::endl;

  // Perform Edge Insertion and deletion for this db.
  const uint64_t vertex_count = db.get_vertex_count();
  std::cout << "Total vertices: " << vertex_count << std::endl;

  // ===== Write Performance =====

  // Concurrent Edge Insert/Delete Performance Test
  const int num_threads = 4;
  const int operations_per_thread = 10000;

  // Statistics
  std::atomic<int> total_operations{0};
  std::atomic<int> insert_count{0};
  std::atomic<int> delete_count{0};
  std::atomic<int> insert_success{0};
  std::atomic<int> delete_success{0};
  std::atomic<int> insert_failures{0};
  std::atomic<int> delete_failures{0};

  auto start_time = std::chrono::high_resolution_clock::now();

  std::cout << "\n=== Starting Concurrent Edge Insert/Delete Test ===" << std::endl;
  std::cout << "Configuration:" << std::endl;
  std::cout << "- Threads: " << num_threads << std::endl;
  std::cout << "- Operations per thread: " << operations_per_thread << std::endl;
  std::cout << "- Total operations: " << (num_threads * operations_per_thread) << std::endl;
  std::cout << "- Vertex range: [0, " << (vertex_count - 1) << "]" << std::endl;
  std::cout << "Starting test..." << std::endl;

  auto edge_operation_task = [&](const tbb::blocked_range<int>& range) {
    // Random number generators for each thread
    std::random_device rd;
    std::mt19937 gen(rd() + range.begin());
    std::uniform_int_distribution<uint64_t> vertex_dis(0, vertex_count - 1);
    std::uniform_int_distribution<int> operation_dis(0, 1); // 0 = insert, 1 = delete

    for (int thread_id = range.begin(); thread_id != range.end(); ++thread_id) {
      int local_insert_success = 0;
      int local_delete_success = 0;
      int local_insert_failures = 0;
      int local_delete_failures = 0;

      for (int op = 0; op < operations_per_thread; ++op) {
        try {
          // Generate random source and destination vertices
          uint64_t src = vertex_dis(gen);
          uint64_t dst = vertex_dis(gen);

          // Randomly choose insert or delete
          bool is_insert = (operation_dis(gen) == 0);

          if (is_insert) {
            // Insert edge
            bool success = db.insert_edge(src, dst);
            insert_count++;
            if (success) {
              local_insert_success++;
            } else {
              local_insert_failures++;
            }
          } else {
            // Delete edge
            bool success = db.delete_edge(src, dst);
            delete_count++;
            if (success) {
              local_delete_success++;
            } else {
              local_delete_failures++;
            }
          }

          total_operations++;

          // Progress report every 200000 operations
          if (op % 200000 == 0 && op > 0) {
            std::cout << "Thread " << thread_id << " completed " << op
                      << " operations (Inserts: " << local_insert_success
                      << ", Deletes: " << local_delete_success << ")" << std::endl;
          }

        } catch (const std::exception& e) {
          std::cout << "Thread " << thread_id << " operation failed: " << e.what() << std::endl;
        } catch (...) {
          std::cout << "Thread " << thread_id << " operation failed with unknown exception"
                    << std::endl;
        }
      }

      // Update global counters
      insert_success += local_insert_success;
      delete_success += local_delete_success;
      insert_failures += local_insert_failures;
      delete_failures += local_delete_failures;

      std::cout << "Thread " << thread_id << " completed all operations - "
                << "Inserts: " << local_insert_success << "/"
                << (local_insert_success + local_insert_failures)
                << ", Deletes: " << local_delete_success << "/"
                << (local_delete_success + local_delete_failures) << std::endl;
    }
  };

  // Execute parallel operations
  tbb::parallel_for(tbb::blocked_range<int>(0, num_threads), edge_operation_task);

  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  // Print comprehensive test results
  std::cout << "\n=== Concurrent Edge Insert/Delete Test Results ===" << std::endl;
  std::cout << "Test Configuration:" << std::endl;
  std::cout << "  Total threads: " << num_threads << std::endl;
  std::cout << "  Operations per thread: " << operations_per_thread << std::endl;
  std::cout << "  Vertex count: " << vertex_count << std::endl;

  std::cout << "\nOperation Statistics:" << std::endl;
  std::cout << "  Total operations: " << total_operations.load() << std::endl;
  std::cout << "  Total insert attempts: " << insert_count.load() << std::endl;
  std::cout << "  Total delete attempts: " << delete_count.load() << std::endl;

  std::cout << "\nSuccess/Failure Breakdown:" << std::endl;
  std::cout << "  Successful inserts: " << insert_success.load() << std::endl;
  std::cout << "  Failed inserts: " << insert_failures.load() << std::endl;
  std::cout << "  Successful deletes: " << delete_success.load() << std::endl;
  std::cout << "  Failed deletes: " << delete_failures.load() << std::endl;

  std::cout << "\nSuccess Rates:" << std::endl;
  if (insert_count.load() > 0) {
    double insert_success_rate = (insert_success.load() * 100.0) / insert_count.load();
    std::cout << "  Insert success rate: " << std::fixed << std::setprecision(2)
              << insert_success_rate << "%" << std::endl;
  }
  if (delete_count.load() > 0) {
    double delete_success_rate = (delete_success.load() * 100.0) / delete_count.load();
    std::cout << "  Delete success rate: " << std::fixed << std::setprecision(2)
              << delete_success_rate << "%" << std::endl;
  }

  std::cout << "\nPerformance Metrics:" << std::endl;
  std::cout << "  Test duration: " << duration.count() << " ms" << std::endl;
  std::cout << "  Operations per second: " << std::fixed << std::setprecision(2)
            << (total_operations.load() * 1000.0 / duration.count()) << std::endl;
  std::cout << "  Inserts per second: " << std::fixed << std::setprecision(2)
            << (insert_success.load() * 1000.0 / duration.count()) << std::endl;
  std::cout << "  Deletes per second: " << std::fixed << std::setprecision(2)
            << (delete_success.load() * 1000.0 / duration.count()) << std::endl;

  std::cout << "\nConcurrency Analysis:" << std::endl;
  double total_expected_ops = num_threads * operations_per_thread;
  double efficiency = (total_operations.load() * 100.0) / total_expected_ops;
  std::cout << "  Expected operations: " << (int) total_expected_ops << std::endl;
  std::cout << "  Actual operations: " << total_operations.load() << std::endl;
  std::cout << "  Execution efficiency: " << std::fixed << std::setprecision(2) << efficiency << "%"
            << std::endl;

  // Additional memory/performance info if available
  std::cout << "\n=== Test Completed Successfully ===" << std::endl;

  // Print additional test information if needed
  // std::cout << "\n=== Perform Consolidation ===" << std::endl;
  // db.consolidate_all_pages();
  db.wait_for_pending_consolidations();
  std::cout << "Consolidation completed." << std::endl;

  // ===== End Write Performance =====
  return 0;
}