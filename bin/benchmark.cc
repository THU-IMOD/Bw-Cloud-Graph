#include "bw_graph/buf/buf_pool.h"
#include "bw_graph/common/logger.h"
#include "bw_graph/common/type.h"
#include "bw_graph/index/vertex_index.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/mem/mem_g0com.h"
#include "bw_graph/storage/disk_manager.h"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <folly/container/F14Set.h>
#include <iostream>
#include <kaminpar.h>
#include <memory>
#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>
#include <vector>

/**
 * RocksDB-based graph storage system
 *
 * This class provides a complete graph storage solution using RocksDB
 * as the underlying storage engine. It stores vertex adjacency lists
 * in binary format for optimal space efficiency.
 */
class RocksDBGraphStorage {
private:
  rocksdb::DB* db_;          // RocksDB database instance
  rocksdb::Options options_; // Database configuration options

public:
  /**
   * Constructor: Initialize RocksDB with optimized settings
   *
   * @param db_path Path to the RocksDB database directory
   *
   * Configures RocksDB with:
   * - SSD-optimized direct I/O settings
   * - Disabled caching for fair performance comparison
   * - Compression and memory optimizations
   */
  RocksDBGraphStorage(const std::string& db_path) {
    // Basic database options
    options_.create_if_missing = true;

    // SSD optimization: Enable direct I/O to bypass OS page cache
    options_.use_direct_reads = true;
    options_.use_direct_io_for_flush_and_compaction = true;
    options_.allow_mmap_reads = false; // Disable memory mapping for direct I/O

    // Memory and write buffer optimizations
    options_.write_buffer_size = 64 * 1024 * 1024;     // 64MB write buffer
    options_.max_write_buffer_number = 3;              // Allow 3 write buffers
    options_.target_file_size_base = 64 * 1024 * 1024; // 64MB SST files

    // File handle optimization
    options_.max_open_files = 1000;

    // Background job optimization
    options_.max_background_jobs = 4;

    // CRITICAL: Disable all caching mechanisms for fair comparison
    rocksdb::BlockBasedTableOptions table_options;
    table_options.block_cache = nullptr;                 // Disable block cache
    table_options.cache_index_and_filter_blocks = false; // Don't cache index/filter blocks
    table_options.pin_l0_filter_and_index_blocks_in_cache = false;
    table_options.pin_top_level_index_and_filter = false;

    // Apply table configuration
    options_.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

    // Ensure row cache is disabled (default behavior)
    options_.row_cache = nullptr;

    // Open the database
    // RocksDB >= 9.x changed DB::Open to take std::unique_ptr<DB>*.
    std::unique_ptr<rocksdb::DB> tmp_db;
    rocksdb::Status status = rocksdb::DB::Open(options_, db_path, &tmp_db);
    if (!status.ok()) {
      throw std::runtime_error("Failed to open RocksDB: " + status.ToString());
    }
    db_ = tmp_db.release();

    std::cout << "RocksDB opened successfully at: " << db_path << " (ALL CACHES DISABLED)"
              << std::endl;
  }

  /**
   * Destructor: Clean up RocksDB resources
   */
  ~RocksDBGraphStorage() { delete db_; }

  /**
   * Store adjacency list for a single vertex
   *
   * @param vertex_id The vertex identifier
   * @param neighbors Vector of neighbor vertex IDs
   *
   * Stores the neighbor list in binary format using vertex ID as key
   * with "v:" prefix to avoid key collisions.
   */
  void store_vertex_neighbors(v_id_t vertex_id, const std::vector<v_id_t>& neighbors) {
    std::string key = "v:" + std::to_string(vertex_id);

    // Serialize neighbor list to binary format for space efficiency
    std::string value;
    value.resize(neighbors.size() * sizeof(v_id_t));
    std::memcpy(value.data(), neighbors.data(), value.size());

    rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), key, value);
    if (!status.ok()) {
      throw std::runtime_error("Failed to store vertex neighbors: " + status.ToString());
    }
  }

  /**
   * Batch store all vertices from the input graph
   *
   * @param graph The input graph in CSR format
   *
   * Uses RocksDB WriteBatch for efficient bulk loading.
   * Processes vertices in batches of 1000 to optimize memory usage
   * and provide progress feedback.
   */
  void store_all_vertices(const mem_sub_com_t& graph) {
    rocksdb::WriteBatch batch;

    std::cout << "Storing " << graph.vertex_count() << " vertices to RocksDB..." << std::endl;

    for (v_id_t vertex_id = 0; vertex_id < graph.vertex_count(); ++vertex_id) {
      std::string key = "v:" + std::to_string(vertex_id);

      // Extract neighbor list from CSR format
      size_t start_offset = graph.offsets[vertex_id];
      size_t end_offset = graph.offsets[vertex_id + 1];
      size_t degree = end_offset - start_offset;

      if (degree > 0) {
        // Directly serialize neighbor data from CSR structure
        const v_id_t* neighbors_ptr = &graph.neighbor_list[start_offset];
        std::string value(reinterpret_cast<const char*>(neighbors_ptr), degree * sizeof(v_id_t));

        batch.Put(key, value);
      } else {
        // Handle vertices with no neighbors
        batch.Put(key, "");
      }

      // Commit batch every 1000 vertices to manage memory usage
      if ((vertex_id + 1) % 1000 == 0) {
        rocksdb::Status status = db_->Write(rocksdb::WriteOptions(), &batch);
        if (!status.ok()) {
          throw std::runtime_error("Failed to write batch: " + status.ToString());
        }
        batch.Clear();

        // Progress reporting every 500,000 vertices
        if ((vertex_id + 1) % 500000 == 0) {
          std::cout << "Stored " << (vertex_id + 1) << " vertices..." << std::endl;
        }
      }
    }

    // Write any remaining entries in the final batch
    if (batch.Count() > 0) {
      rocksdb::Status status = db_->Write(rocksdb::WriteOptions(), &batch);
      if (!status.ok()) {
        throw std::runtime_error("Failed to write final batch: " + status.ToString());
      }
    }

    // Force write-ahead log flush to ensure durability
    db_->FlushWAL(true);

    std::cout << "All vertices stored successfully!" << std::endl;
  }

  /**
   * Retrieve adjacency list for a single vertex
   *
   * @param vertex_id The vertex identifier to query
   * @return Vector of neighbor vertex IDs
   *
   * Deserializes the binary-stored neighbor list back to vector format.
   * Returns empty vector if vertex has no neighbors or doesn't exist.
   */
  std::vector<v_id_t> get_vertex_neighbors(v_id_t vertex_id) {
    std::string key = "v:" + std::to_string(vertex_id);
    std::string value;

    // Optimize read performance by skipping checksum verification
    rocksdb::ReadOptions read_options;
    read_options.verify_checksums = false;

    rocksdb::Status status = db_->Get(read_options, key, &value);

    if (!status.ok()) {
      if (status.IsNotFound()) {
        return {}; // Return empty list for non-existent vertices
      }
      throw std::runtime_error("Failed to get vertex neighbors: " + status.ToString());
    }

    // Deserialize binary data back to neighbor vector
    std::vector<v_id_t> neighbors;
    if (!value.empty()) {
      size_t neighbor_count = value.size() / sizeof(v_id_t);
      neighbors.resize(neighbor_count);
      std::memcpy(neighbors.data(), value.data(), value.size());
    }

    return neighbors;
  }

  /**
   * Load all vertices from RocksDB into memory
   *
   * @param vertex_count Total number of vertices in the graph
   * @return Vector of neighbor lists for all vertices
   *
   * This method performs a full graph scan, loading all adjacency
   * lists into memory. Used for full-graph loading performance testing.
   */
  std::vector<std::vector<v_id_t>> load_all_vertices(size_t vertex_count) {
    std::vector<std::vector<v_id_t>> all_neighbors(vertex_count);

    std::cout << "Loading all " << vertex_count << " vertices from RocksDB..." << std::endl;

    for (v_id_t vertex_id = 0; vertex_id < vertex_count; ++vertex_id) {
      all_neighbors[vertex_id] = get_vertex_neighbors(vertex_id);

      // Progress reporting every 500,000 vertices
      if ((vertex_id + 1) % 500000 == 0) {
        std::cout << "Loaded " << (vertex_id + 1) << " vertices..." << std::endl;
      }
    }

    std::cout << "All vertices loaded successfully!" << std::endl;
    return all_neighbors;
  }

  /**
   * Print RocksDB internal statistics
   *
   * Displays detailed performance metrics including cache hits,
   * I/O statistics, and compaction information.
   */
  void print_stats() {
    std::string stats;
    db_->GetProperty("rocksdb.stats", &stats);
    std::cout << "RocksDB Statistics:" << std::endl << stats << std::endl;
  }
};

/**
 * Main function: Performance comparison between RocksDB and page-based storage
 *
 * The test follows this workflow:
 * 1. Load input graph and set up page-based storage system
 * 2. Set up RocksDB storage system with disabled caching
 * 3. Perform full-graph loading tests on both systems
 * 4. Compare performance and validate correctness
 */
int main() {
  // Load input graph file
  std::string test_file = "data/rn.graph";
  mem_sub_com_t com_graph(test_file, true);

  std::cout << "Graph loaded: " << com_graph.vertex_count() << " vertices, "
            << com_graph.edge_count() << " edges" << std::endl;

  // =========================
  // 1. Page-based storage system setup
  // =========================

  // Validate data type sizes for KaMinPar compatibility
  assert(sizeof(kaminpar::shm::NodeID) == 8);
  assert(sizeof(kaminpar::shm::EdgeID) == 8);

  // Extract graph dimensions
  size_t n = com_graph.vertex_count();
  size_t m = com_graph.edge_count();

  // Convert graph to KaMinPar format for partitioning
  std::vector<kaminpar::shm::EdgeID> xadj(n + 1);
  std::vector<kaminpar::shm::NodeID> adjncy(m);

  // Copy CSR offset array
  for (size_t i = 0; i <= n; ++i) {
    xadj[i] = static_cast<kaminpar::shm::EdgeID>(com_graph.offsets[i]);
  }

  // Copy CSR adjacency array
  for (size_t i = 0; i < m; ++i) {
    adjncy[i] = static_cast<kaminpar::shm::NodeID>(com_graph.neighbor_list[i]);
  }

  // Calculate vertex weights based on degree (degree + 1 to avoid zero weights)
  std::vector<kaminpar::shm::NodeWeight> node_weights(n);
  kaminpar::shm::NodeWeight total_weight = 0;

  for (size_t i = 0; i < n; ++i) {
    size_t degree = com_graph.offsets[i + 1] - com_graph.offsets[i];
    node_weights[i] = static_cast<kaminpar::shm::NodeWeight>(1 + degree);
    total_weight += node_weights[i];
  }

  // Calculate number of partitions (target: ~600 weight units per partition)
  kaminpar::shm::BlockID num_partitions =
      static_cast<kaminpar::shm::BlockID>((total_weight + 600 - 1) / 600);

  // Initialize KaMinPar partitioner with 8 threads
  kaminpar::KaMinPar shm_partitioner(8, kaminpar::shm::create_default_context());
  shm_partitioner.set_output_level(kaminpar::OutputLevel::QUIET);
  shm_partitioner.borrow_and_mutate_graph(xadj, adjncy, node_weights);
  shm_partitioner.set_k(num_partitions);

  // Set maximum weight per partition
  std::vector<kaminpar::shm::BlockWeight> max_block_weights(num_partitions, 600);
  shm_partitioner.set_absolute_max_block_weights(max_block_weights);

  // Perform graph partitioning
  std::vector<kaminpar::shm::BlockID> partition(n);
  graph_partition_t graph_partition(num_partitions);

  std::cout << "Starting graph partitioning..." << std::endl;
  kaminpar::shm::EdgeWeight cut = shm_partitioner.compute_partition(partition);
  std::cout << "Graph partitioning completed!" << std::endl;
  std::cout << "Cut size: " << cut << std::endl;

  // Group vertices by their assigned partitions
  for (size_t i = 0; i < n; ++i) {
    kaminpar::shm::BlockID part = partition[i];
    graph_partition[part].push_back(static_cast<v_id_t>(i));
  }

  // Create CSR pages and vertex index for efficient access
  std::vector<csr_page_t*> page_list(graph_partition.size());
  std::vector<v_id_t> giant_vertex_ids;
  vertex_index_t vertex_index(com_graph, graph_partition, page_list, giant_vertex_ids);

  // Initialize disk manager and write all pages to storage
  const std::filesystem::path test_db_file = bw_graph::csr_db_path("rn");
  disk_manager_t disk_manager(test_db_file);

  page_no_t current_page_no = 0;
  for (auto csr_page : page_list) {
    disk_manager.write_page(current_page_no++, csr_page->get_data());
  }
  std::cout << "CSR pages written to disk successfully!" << std::endl;
  std::cout << "Giant Vertex Count: " << giant_vertex_ids.size() << std::endl;

  // =========================
  // 2. RocksDB storage system setup
  // =========================

  RocksDBGraphStorage rocksdb_storage(
      bw_graph::storage_path(bw_graph::META_PREFIX, "rn_rocksdb").string());

  // Measure time to store all vertices in RocksDB
  auto rocksdb_store_start = std::chrono::high_resolution_clock::now();
  rocksdb_storage.store_all_vertices(com_graph);
  auto rocksdb_store_end = std::chrono::high_resolution_clock::now();
  auto rocksdb_store_duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(rocksdb_store_end - rocksdb_store_start)
          .count();

  std::cout << "RocksDB storage time: " << rocksdb_store_duration << " ms" << std::endl;

  // =========================
  // 3. Full graph loading performance test
  // =========================

  std::cout << "\n=== Full Graph Loading Performance Test ===" << std::endl;

  // Test 1: RocksDB full graph loading (no caching)
  std::cout << "\nTesting RocksDB full graph loading (no cache)..." << std::endl;
  auto rocksdb_load_start = std::chrono::high_resolution_clock::now();
  std::vector<std::vector<v_id_t>> rocksdb_all_neighbors = rocksdb_storage.load_all_vertices(n);
  auto rocksdb_load_end = std::chrono::high_resolution_clock::now();
  auto rocksdb_load_duration =
      std::chrono::duration_cast<std::chrono::microseconds>(rocksdb_load_end - rocksdb_load_start)
          .count();

  // Test 2: Page-based system full graph loading with intelligent page caching
  std::cout << "\nTesting page-based full graph loading (with page caching)..." << std::endl;

  auto buf_pool = buf_pool_t<csr_page_t>::buf_pool_init(4, 4, bw_graph::BW_GRAPH_PAGE_SIZE);
  // Store neighbor spans for all vertices
  std::vector<std::vector<v_id_t>> page_based_all_neighbors(n);
  size_t total_time = 0; // Accumulate only actual I/O time

  // Process each vertex and load its containing page if necessary
  for (v_id_t vertex_id = 0; vertex_id < n; ++vertex_id) {
    // Look up which page contains this vertex
    auto [page_no, offset] = vertex_index.get_vertex_location(vertex_id);

    auto page_load_start = std::chrono::high_resolution_clock::now();
    csr_page_t* current_page = buf_pool->buf_page_read(page_no, &disk_manager);
    auto page_load_end = std::chrono::high_resolution_clock::now();
    auto page_load_duration =
        std::chrono::duration_cast<std::chrono::microseconds>(page_load_end - page_load_start)
            .count();
    total_time += page_load_duration;

    // Extract neighbor span for this vertex from the loaded page
    auto neighbors_span = current_page->get_neighbors(offset);
    page_based_all_neighbors[vertex_id].assign(neighbors_span.begin(), neighbors_span.end());
    current_page->r_unlatch();

    if (current_page == nullptr) {
      bw_graph::logger::log_error("Logic Error: Page not found in buffer pool.");
      std::cout << "Page not found in buffer pool for vertex " << vertex_id
                << " (page_no: " << page_no << ")" << std::endl;
    }

    // Progress reporting every 500,000 vertices
    if ((vertex_id + 1) % 500000 == 0) {
      std::cout << "Processed " << (vertex_id + 1) << " vertices..." << std::endl;
    }
  }

  std::cout << "Page-based full loading time: " << total_time << " us" << std::endl;

  // =========================
  // 4. Results comparison and validation
  // =========================

  std::cout << "\n=== Results Comparison ===" << std::endl;
  std::cout << "RocksDB total loading time: " << rocksdb_load_duration << " us" << std::endl;
  std::cout << "Page-based total loading time: " << total_time << " us" << std::endl;

  // Correctness validation using random sampling
  std::cout << "\n=== Correctness Verification ===" << std::endl;
  bool all_correct = true;
  const int sample_vertices = std::min(100, static_cast<int>(n));

  for (int i = 0; i < sample_vertices; ++i) {
    // Sample vertices uniformly across the graph
    v_id_t vertex_id = static_cast<v_id_t>(i * n / sample_vertices);

    // Get ground truth from original graph
    std::vector<v_id_t> ground_truth = com_graph.get_neighbors(vertex_id);

    // Get results from both storage systems
    std::vector<v_id_t>& rocksdb_result = rocksdb_all_neighbors[vertex_id];
    std::vector<v_id_t>& page_result = page_based_all_neighbors[vertex_id];

    // Compare vector with vector using standard equality
    bool rocksdb_match =
        (ground_truth.size() == rocksdb_result.size()) &&
        std::equal(ground_truth.begin(), ground_truth.end(), rocksdb_result.begin());

    // Compare vector with span using std::equal
    bool page_match = (ground_truth.size() == page_result.size()) &&
                      std::equal(ground_truth.begin(), ground_truth.end(), page_result.begin());

    // Report any mismatches found
    if (!rocksdb_match || !page_match) {
      std::cout << "Mismatch at vertex " << vertex_id << std::endl;
      std::cout << "  Ground truth size: " << ground_truth.size() << std::endl;
      std::cout << "  RocksDB size: " << rocksdb_result.size() << std::endl;
      std::cout << "  Page-based size: " << page_result.size() << std::endl;
      all_correct = false;
      std::cout << "Page-based neighbors: ";
      for (auto neighbor : page_result) {
        std::cout << neighbor << "->";
      }
      std::cout << std::endl;
      std::cout << "  Ground truth neighbors: ";
      for (auto neighbor : ground_truth) {
        std::cout << neighbor << "->";
      }
      std::cout << std::endl;
      break;
    }
  }

  std::cout << "Sample verification (" << sample_vertices
            << " vertices): " << (all_correct ? "PASSED" : "FAILED") << std::endl;

  return 0;
}
