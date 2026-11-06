#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <sys/types.h>
#include <vector>

// Generate timestamp string in yyyymmddHHMMSS format
static std::string generate_timestamp_log() {
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  std::tm tm_now;
  localtime_r(&time_t_now, &tm_now);

  std::ostringstream oss;
  oss << std::put_time(&tm_now, "%Y%m%d%H%M%S");
  return oss.str();
}

// Generate log filename: {dataset}-storage-yyyymmddHHMMSS.log
static std::string generate_log_filename(const std::string& graph_name) {
  std::ostringstream oss;
  oss << graph_name << "-storage-" << generate_timestamp_log() << ".log";
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

// Write storage results to log file
static void write_storage_results(std::ofstream& log_file, const std::string& graph_name,
                                  size_t page_size, uint64_t db_raw_size, uint64_t db_total_pages,
                                  uint64_t db_valid_pages, uint64_t db_used_bytes,
                                  uint64_t giant_used_bytes, uint64_t total_used_bytes) {
  log_file << "Storage Analysis Report\n";
  log_file << "Experiment Timestamp: " << generate_timestamp_log() << "\n";
  log_file << "Dataset: " << graph_name << "\n\n";

  log_file << "--- " << graph_name << ".db (Base Page Store) ---\n";
  log_file << "Page size: " << page_size << " bytes\n";
  log_file << "Total pages (raw): " << db_total_pages << "\n";
  log_file << "Raw file size: " << db_raw_size << " bytes\n";
  log_file << "Valid pages: " << db_valid_pages << "\n";
  log_file << "Used storage: " << db_used_bytes << " bytes\n\n";

  log_file << "--- " << graph_name << "_giantdb (Giant Vertex Store) ---\n";
  log_file << "Used storage: " << giant_used_bytes << " bytes\n\n";

  log_file << "--- Total ---\n";
  log_file << graph_name << ".db used: " << db_used_bytes << " bytes\n";
  log_file << graph_name << "_giantdb used: " << giant_used_bytes << " bytes\n";
  log_file << "Total used: " << total_used_bytes << " bytes\n";
}

/**
 * @brief Count the number of valid (non-zero) pages from the end of a db file.
 *
 * The .db file is pre-allocated in chunks, so the tail may contain many
 * all-zero pages. We scan backwards to find the last page that has any
 * non-zero byte, then report valid_pages * page_size as the real storage used.
 *
 * @param path     Path to the .db file
 * @param page_size Size of each page in bytes
 * @return Number of bytes actually used by valid pages, 0 on error
 */
uint64_t get_db_used_bytes(const std::filesystem::path& path, size_t page_size) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    std::cerr << "Error: cannot open " << path << std::endl;
    return 0;
  }

  std::streamoff file_size = file.tellg();
  if (file_size <= 0) {
    return 0;
  }

  uint64_t total_pages = static_cast<uint64_t>(file_size) / page_size;
  if (total_pages == 0) {
    return static_cast<uint64_t>(file_size);
  }

  std::vector<char> buf(page_size);

  // Scan backwards from the last page to find the last non-zero page
  for (uint64_t i = total_pages; i > 0; --i) {
    std::streamoff offset = static_cast<std::streamoff>((i - 1) * page_size);
    file.seekg(offset);
    file.read(buf.data(), static_cast<std::streamsize>(page_size));

    bool all_zero = true;
    for (size_t j = 0; j < page_size; ++j) {
      if (buf[j] != 0) {
        all_zero = false;
        break;
      }
    }

    if (!all_zero) {
      // Page (i-1) is the last valid page; i pages are valid
      return i * page_size;
    }
  }

  return 0; // All pages are zero (empty file)
}

/**
 * @brief Recursively sum the sizes of all regular files in a directory.
 *
 * @param dir_path Path to the directory
 * @return Total bytes of all regular files, 0 if directory doesn't exist
 */
uint64_t get_dir_used_bytes(const std::filesystem::path& dir_path) {
  if (!std::filesystem::exists(dir_path) || !std::filesystem::is_directory(dir_path)) {
    return 0;
  }

  uint64_t total = 0;
  try {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
      if (entry.is_regular_file()) {
        total += entry.file_size();
      }
    }
  } catch (const std::filesystem::filesystem_error& e) {
    std::cerr << "Error reading directory: " << e.what() << std::endl;
  }
  return total;
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

  // Ensure log directory exists and open log file
  ensure_log_directory();
  std::string log_filename = generate_log_filename(g_name);
  std::string log_path = "./logs/" + log_filename;
  std::ofstream log_file(log_path);
  if (!log_file.is_open()) {
    std::cerr << "Failed to open log file: " << log_path << std::endl;
    return 1;
  }
  std::cout << "Log file: " << log_path << std::endl;

  std::cout << "Creating database.." << std::endl;
  auto db =
      bw_graph_db_t(g_name, bw_graph::BW_GRAPH_REBUILD_INDEX, bw_graph::BW_GRAPH_REDO_PARTITION,
                    false, bw_graph::BW_GRAPH_ID_AWARE, true, true);
  assert(db.graph_name_ == g_name);
  std::cout << "Database created successfully" << std::endl;

  const size_t page_size = bw_graph::BW_GRAPH_PAGE_SIZE;

  // ===== .db file: count valid pages =====
  std::filesystem::path db_file = bw_graph::csr_db_path(g_name);
  uint64_t db_raw_size = 0;
  uint64_t db_used_bytes = 0;

  if (std::filesystem::exists(db_file)) {
    db_raw_size = std::filesystem::file_size(db_file);
    uint64_t total_pages = db_raw_size / page_size;
    db_used_bytes = get_db_used_bytes(db_file, page_size);
    uint64_t valid_pages = db_used_bytes / page_size;

    std::cout << "\n=== " << g_name << ".db (Base Page Store) ===" << std::endl;
    std::cout << "  Page size       : " << format_size(page_size) << std::endl;
    std::cout << "  Total pages     : " << total_pages
              << "  (raw file: " << format_size(db_raw_size) << ")" << std::endl;
    std::cout << "  Valid pages     : " << valid_pages << std::endl;
    std::cout << "  Used storage    : " << format_size(db_used_bytes) << " (" << db_used_bytes
              << " bytes)" << std::endl;
  } else {
    std::cout << "\nWarning: .db file not found: " << db_file << std::endl;
  }

  // ===== _giantdb directory: sum all file sizes =====
  std::filesystem::path giant_dir = bw_graph::giant_db_path(g_name);
  uint64_t giant_used_bytes = 0;

  if (std::filesystem::exists(giant_dir)) {
    giant_used_bytes = get_dir_used_bytes(giant_dir);
    std::cout << "\n=== " << g_name << "_giantdb (Giant Vertex Store) ===" << std::endl;
    std::cout << "  Directory       : " << giant_dir << std::endl;
    std::cout << "  Used storage    : " << format_size(giant_used_bytes) << " (" << giant_used_bytes
              << " bytes)" << std::endl;
  } else {
    std::cout << "\nWarning: _giantdb directory not found: " << giant_dir << std::endl;
  }

  // ===== Total =====
  uint64_t total_used = db_used_bytes + giant_used_bytes;
  std::cout << "\n=== Total Storage Used ===" << std::endl;
  std::cout << "  " << g_name << ".db          : " << format_size(db_used_bytes) << std::endl;
  std::cout << "  " << g_name << "_giantdb     : " << format_size(giant_used_bytes) << std::endl;
  std::cout << "  Total             : " << format_size(total_used) << " (" << total_used
            << " bytes)" << std::endl;
  std::cout << "==========================" << std::endl;

  // ===== Write results to log file =====
  uint64_t db_total_pages = (db_raw_size > 0) ? db_raw_size / page_size : 0;
  uint64_t db_valid_pages = db_used_bytes / page_size;
  write_storage_results(log_file, g_name, page_size, db_raw_size, db_total_pages, db_valid_pages,
                        db_used_bytes, giant_used_bytes, total_used);
  log_file.close();
  std::cout << "Results written to: " << log_path << std::endl;

  return 0;
}
