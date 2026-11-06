#ifndef BW_GRAPH_BUF_MULTISCALE_BUF_POOL_H
#define BW_GRAPH_BUF_MULTISCALE_BUF_POOL_H

#include "bw_graph/common/multiscale_buffer_config.h"
#include "bw_graph/common/latch.h"
#include "bw_graph/storage/multiscale_disk_manager.h"

#include <memory>
#include <shared_mutex>
#include <stdexcept>

namespace bw_graph {
namespace multiscale_buf_detail {
struct state_t;
struct frame_t;
struct chunk_t;
// Reuse the project's compact content latch; guards retain standard RAII locking.
struct content_latch_t {
  void lock() { latch.w_lock(); }
  void unlock() { latch.w_unlock(); }
  bool try_lock() { return latch.try_w_lock(); }
  void lock_shared() { latch.r_lock(); }
  void unlock_shared() { latch.r_unlock(); }
  bool try_lock_shared() { return latch.try_r_lock(); }
private:
  rw_latch_t latch;
};
}
// Borrowed hint: usable only with the originating live pool. Identity is rechecked after pin.
struct page_frame_hint_t {
  std::atomic<multiscale_buf_detail::frame_t*> frame{nullptr};
};
class read_page_guard_t;
class write_page_guard_t;

class buffer_full_t : public std::runtime_error {
public:
  buffer_full_t() : std::runtime_error("All frames in this page space are in use") {}
};

// Move-only pin, safe to transfer between threads. It does not lock page contents.
// Guards and pins retain chunk storage; the disk manager must outlive the pool and its requests.
class page_pin_t {
public:
  page_pin_t() = default;
  ~page_pin_t();
  page_pin_t(page_pin_t&&) noexcept;
  page_pin_t& operator=(page_pin_t&&) noexcept;
  page_pin_t(const page_pin_t&) = delete;
  page_pin_t& operator=(const page_pin_t&) = delete;
  explicit operator bool() const { return frame_ != nullptr; }
  page_id_t page_id() const;
  read_page_guard_t read() &&;
  write_page_guard_t write() &&;

private:
  friend struct multiscale_buf_detail::state_t;
  friend class multiscale_buf_pool_t;
  friend class read_page_guard_t;
  friend class write_page_guard_t;
  page_pin_t(std::shared_ptr<multiscale_buf_detail::chunk_t>, multiscale_buf_detail::frame_t*);
  void reset() noexcept;
  std::shared_ptr<multiscale_buf_detail::chunk_t> chunk_;
  multiscale_buf_detail::frame_t* frame_{nullptr};
  bool dirty_{false};
};

// Guards must be destroyed on the thread that acquired their content lock.
// Declaration order ensures content unlock occurs BEFORE unpin/dirty publication.
class read_page_guard_t {
public:
  explicit read_page_guard_t(page_pin_t pin);
  read_page_guard_t(read_page_guard_t&&) noexcept = default;
  read_page_guard_t& operator=(read_page_guard_t&&) = delete;
  page_id_t page_id() const { return pin_.page_id(); }
  std::span<const std::byte> bytes() const;
private:
  page_pin_t pin_;
  std::shared_lock<multiscale_buf_detail::content_latch_t> lock_;
};
class write_page_guard_t {
public:
  explicit write_page_guard_t(page_pin_t pin);
  write_page_guard_t(write_page_guard_t&&) noexcept = default;
  write_page_guard_t& operator=(write_page_guard_t&&) = delete;
  page_id_t page_id() const { return pin_.page_id(); }
  std::span<std::byte> bytes();
private:
  page_pin_t pin_;
  std::unique_lock<multiscale_buf_detail::content_latch_t> lock_;
};

struct buffer_read_request_t {
  page_id_t page_id;
  std::future<page_pin_t> result;
};
struct buffer_group_stats_t {
  size_t frame_count{0};
  size_t chunk_bytes{0}; // Control structures + padding + frames; excludes maps/requests.
  size_t pinned_frames{0};
  uint64_t hits{0}, misses{0}, coalesced{0}, disk_reads{0};
  uint64_t evictions{0}, writebacks{0}, failures{0};
};

// One manager, six chunk groups. No buf_pool_t composition and no private I/O workers.
// No whole-page parsing/copy on fetch. READY means raw bytes are available; typed views
// perform header and per-access checks while the caller's guard protects their lifetime.
class multiscale_buf_pool_t {
public:
  explicit multiscale_buf_pool_t(multiscale_disk_manager_t& disk,
                                 multiscale_buf_options_t options = {});
  ~multiscale_buf_pool_t();
  multiscale_buf_pool_t(const multiscale_buf_pool_t&) = delete;
  multiscale_buf_pool_t& operator=(const multiscale_buf_pool_t&) = delete;

  page_pin_t pin_page(page_id_t id);
  // Nonblocking resident lookup; never reserves a frame or submits I/O.
  page_pin_t try_pin_page(page_id_t id);
  read_page_guard_t fetch_read(page_id_t id);
  read_page_guard_t fetch_read(page_id_t id, page_frame_hint_t& hint);
  write_page_guard_t fetch_write(page_id_t id);
  write_page_guard_t fetch_write(page_id_t id, page_frame_hint_t& hint);
  // Only for a freshly allocated, unpublished disk ID. Zeroes a frame without
  // reading the new slot. Caller initializes it before publishing its identity.
  write_page_guard_t new_page(page_id_t id);
  // Queue admission and dirty victim writeback can block submission; the read is async.
  buffer_read_request_t fetch_async(page_id_t id);
  // One result/error per input. Window requests to available frames; never wait for
  // capacity while holding other pins. Full/disabled groups return errors via futures.
  std::vector<buffer_read_request_t> fetch_batch(std::span<const page_id_t> ids);
  void prefetch(page_id_t id); // Best effort; discarded result releases the pin at completion.
  // Release content guards before flushing; flush acquires the page write latch.
  void flush_page(page_id_t id); // Writes a resident dirty page; does not fsync.
  void flush_all(); // Caller quiesces writers; writes resident dirties and syncs the store.
  // Stops new fetches, drains accepted loads, returns false if external pins remain.
  // Release all guards/results then call again. Explicit close reports write/sync failures.
  // The destructor is best effort: outstanding pins or I/O errors prevent a full flush.
  bool close();
  buffer_group_stats_t stats(page_space_t space) const;

private:
  std::shared_ptr<multiscale_buf_detail::state_t> state_;
};
} // namespace bw_graph
#endif
