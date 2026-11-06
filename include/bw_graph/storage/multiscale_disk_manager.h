#ifndef BW_GRAPH_STORAGE_MULTISCALE_DISK_MANAGER_H
#define BW_GRAPH_STORAGE_MULTISCALE_DISK_MANAGER_H

#include "bw_graph/storage/page_id.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <thread>
#include <vector>
#include <variant>

namespace bw_graph {

// Move-only raw page owner. No CSR/Index/Delta parsing or buffer-pool state.
// 4096-byte alignment supports Linux O_DIRECT and future ownership transfer.
class disk_page_t {
public:
  disk_page_t(page_id_t id, size_t size);
  disk_page_t(disk_page_t&& other) noexcept;
  disk_page_t& operator=(disk_page_t&& other) noexcept;
  disk_page_t(const disk_page_t&) = delete;
  disk_page_t& operator=(const disk_page_t&) = delete;
  page_id_t page_id() const { return id_; }
  std::span<std::byte> bytes() { return {data_.get(), size_}; }
  std::span<const std::byte> bytes() const { return {data_.get(), size_}; }

private:
  struct free_t { void operator()(std::byte* p) const { std::free(p); } };
  page_id_t id_;
  size_t size_{0};
  std::unique_ptr<std::byte, free_t> data_;
};

struct page_read_request_t {
  page_id_t page_id;
  std::future<disk_page_t> result; // get() transfers ownership or reports the I/O exception.
};

struct multiscale_disk_options_t {
  uint32_t index_page_size{16384};
  uint32_t delta_page_size{4096};
  size_t io_workers{4};
  size_t queue_capacity{256}; // Pending jobs, excluding active workers and returned results.
  bool bypass_cache{true};   // macOS F_NOCACHE; Linux O_DIRECT, no silent fallback.
};

// Independent experimental store: exactly six data fds, no legacy disk_manager objects.
// Each file has a 4 KiB versioned header, then fixed-size slots. Allocation is append-only:
// ftruncate reserves a zero page; complete file length recovers allocated count on reopen.
// Single owning manager/process per store (flock). Same-page read/write synchronization
// belongs to the caller. No crash-atomic writes, page reclamation, or format migration.
class multiscale_disk_manager_t {
public:
  static constexpr uint32_t file_header_bytes = 4096;
  explicit multiscale_disk_manager_t(
      const std::filesystem::path& directory, multiscale_disk_options_t options = {});
  ~multiscale_disk_manager_t();
  multiscale_disk_manager_t(const multiscale_disk_manager_t&) = delete;
  multiscale_disk_manager_t& operator=(const multiscale_disk_manager_t&) = delete;

  uint32_t page_size(page_space_t space) const;
  uint64_t get_page_count(page_space_t space) const;
  uint64_t get_sync_count(page_space_t space) const; // Successful file sync calls.
  page_id_t allocate_page(page_space_t space);
  page_id_t allocate_pages(page_space_t space, uint32_t count);
  // Exact-size buffers required. Unaligned buffers use an aligned bounce buffer only
  // on Linux O_DIRECT; read_page(id) returns aligned storage without this extra copy.
  void read_page(page_id_t id, std::span<std::byte> destination) const;
  disk_page_t read_page(page_id_t id) const;
  void write_page(page_id_t id, std::span<const std::byte> source);
  void write_page(const disk_page_t& page) { write_page(page.page_id(), page.bytes()); }

  // One API submission, one future per page, results in input order including duplicates.
  // Batches execute concurrently via workers (not one OS syscall / not io_uring).
  // Submission applies backpressure when the bounded queue is full. No cancellation.
  page_read_request_t read_page_async(page_id_t id);
  std::vector<page_read_request_t> read_pages_async(std::span<const page_id_t> ids);
  std::vector<disk_page_t> read_pages(std::span<const page_id_t> ids);

  // Borrowed destination must remain valid until completion. Aligned/exact-size only:
  // this path never allocates a page or copies its contents. Reuses the I/O workers.
  // Throws before acceptance (no callback), otherwise calls completion exactly once,
  // outside queue/lifecycle locks. Completion must not throw or block on this executor.
  void read_into_async(page_id_t id, std::span<std::byte> destination,
                       std::function<void(std::exception_ptr)> completion);

  // Caller must quiesce writes for a complete flush boundary; normal writes only
  // finish the transfer. New file directory entries are not separately fsynced.
  void flush();
  void shutdown(); // Stop submissions, drain/join workers, sync and close all six fds.
  // Futures remain valid after shutdown/destruction. Explicit shutdown reports sync errors;
  // the destructor cannot report errors. No method may race object destruction.

private:
  struct space_state_t {
    int fd{-1};
    uint32_t page_size{0};
    std::atomic<uint64_t> count{0};
    std::mutex allocation_mutex;
    // First flush also synchronizes files opened from an earlier process.
    std::atomic<uint64_t> write_epoch{1}, sync_count{0};
    uint64_t synced_epoch{0}; // Protected by sync_mutex.
    std::mutex sync_mutex;
  };
  using owned_read_job_t = std::packaged_task<disk_page_t()>;
  struct inplace_read_job_t {
    page_id_t id;
    std::span<std::byte> destination;
    std::function<void(std::exception_ptr)> completion;
  };
  using read_job_t = std::variant<owned_read_job_t, inplace_read_job_t>;
  static size_t space_index(page_space_t space);
  void check_open() const;
  void worker_loop();
  void sync_files();
  void close_files() noexcept;

  multiscale_disk_options_t options_;
  std::array<space_state_t, 6> spaces_;
  mutable std::shared_mutex lifecycle_mutex_;
  bool closed_{false};
  std::mutex shutdown_mutex_;
  std::mutex queue_mutex_;
  std::condition_variable work_ready_;
  std::condition_variable queue_available_;
  std::deque<read_job_t> queue_;
  bool stopping_{false};
  std::vector<std::thread> workers_;
};

} // namespace bw_graph
#endif
