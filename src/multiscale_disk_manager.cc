#include "bw_graph/storage/multiscale_disk_manager.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bw_graph {
namespace {
constexpr std::array<const char*, 6> file_names{
    "csr_4k.pages", "csr_32k.pages", "csr_256k.pages", "csr_2m.pages", "index.pages",
    "delta.pages"};
constexpr char file_magic[] = "BWMSIO01";

[[noreturn]] void io_error(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

// Full transfer, including short successful transfers and EINTR. EOF is an error,
// never a synthesized zero page. Caller supplies a checked off_t and page size.
void transfer(int fd, std::byte* data, size_t size, off_t offset, bool write) {
  size_t done = 0;
  while (done < size) {
    const ssize_t n = write ? ::pwrite(fd, data + done, size - done, offset + done)
                            : ::pread(fd, data + done, size - done, offset + done);
    if (n < 0) {
      if (errno == EINTR) continue;
      io_error(write ? "pwrite page" : "pread page");
    }
    if (n == 0) throw std::runtime_error(write ? "Zero-length page write" : "Truncated page");
    done += static_cast<size_t>(n);
  }
}

void put32(std::span<std::byte> data, size_t offset, uint32_t value) {
  for (unsigned b = 0; b < 4; ++b) data[offset + b] = std::byte((value >> (8 * b)) & 255);
}
uint32_t get32(std::span<const std::byte> data, size_t offset) {
  uint32_t value = 0;
  for (unsigned b = 0; b < 4; ++b) {
    value |= std::to_integer<uint32_t>(data[offset + b]) << (8 * b);
  }
  return value;
}

off_t page_offset(page_id_t id, uint32_t size) {
  const uint64_t offset = uint64_t(id.slot()) * size +
                          multiscale_disk_manager_t::file_header_bytes;
  if (offset + size > uint64_t(std::numeric_limits<off_t>::max())) {
    throw std::overflow_error("Page offset exceeds off_t");
  }
  return static_cast<off_t>(offset);
}

bool needs_bounce(const void* data, bool bypass) {
#ifdef __linux__
  return bypass && reinterpret_cast<uintptr_t>(data) % 4096 != 0;
#else
  (void)data;
  (void)bypass;
  return false;
#endif
}
} // namespace

disk_page_t::disk_page_t(page_id_t id, size_t size) : id_(id), size_(size) {
  if (!id.valid() || size == 0 || size > 2097152 || size % 4096 != 0) {
    throw std::invalid_argument("Invalid raw page ID or capacity");
  }
  void* ptr = nullptr;
  const int error = ::posix_memalign(&ptr, 4096, size);
  if (error != 0) throw std::system_error(error, std::generic_category(), "Page allocation");
  data_.reset(static_cast<std::byte*>(ptr));
}

disk_page_t::disk_page_t(disk_page_t&& other) noexcept
    : id_(std::exchange(other.id_, page_id_t{})), size_(std::exchange(other.size_, 0)),
      data_(std::move(other.data_)) {}

disk_page_t& disk_page_t::operator=(disk_page_t&& other) noexcept {
  if (this != &other) {
    id_ = std::exchange(other.id_, page_id_t{});
    size_ = std::exchange(other.size_, 0);
    data_ = std::move(other.data_);
  }
  return *this;
}

size_t multiscale_disk_manager_t::space_index(page_space_t space) {
  const auto i = static_cast<size_t>(space);
  if (i >= 6) throw std::invalid_argument("Invalid page space");
  return i;
}

multiscale_disk_manager_t::multiscale_disk_manager_t(
    const std::filesystem::path& directory, multiscale_disk_options_t options)
    : options_(options) {
  if (options.io_workers == 0 || options.queue_capacity == 0) {
    throw std::invalid_argument("I/O workers and queue capacity must be positive");
  }
  const std::array<uint32_t, 6> sizes{
      4096, 32768, 262144, 2097152, options.index_page_size, options.delta_page_size};
  for (auto size : sizes) {
    if (size == 0 || size > 2097152 || size % 4096 != 0) {
      throw std::invalid_argument("Page sizes must be multiples of 4 KiB, at most 2 MiB");
    }
  }
  std::filesystem::create_directories(directory);
  try {
    for (size_t i = 0; i < spaces_.size(); ++i) {
      auto& space = spaces_[i];
      space.page_size = sizes[i];
      int flags = O_RDWR | O_CREAT | O_CLOEXEC;
#ifdef __linux__
      if (options.bypass_cache) flags |= O_DIRECT;
#endif
      space.fd = ::open((directory / file_names[i]).c_str(), flags, 0644);
      if (space.fd < 0) io_error("Open page space");
      if (::flock(space.fd, LOCK_EX | LOCK_NB) != 0) io_error("Page space already in use");
#ifdef __APPLE__
      if (options.bypass_cache && ::fcntl(space.fd, F_NOCACHE, 1) != 0) {
        io_error("F_NOCACHE");
      }
#elif defined(__linux__)
      // Advisory only: failure does not change the configured buffered/direct mode.
      (void)::posix_fadvise(space.fd, 0, 0, POSIX_FADV_RANDOM);
#endif
      struct stat st{};
      if (::fstat(space.fd, &st) != 0) io_error("Stat page space");
      auto id = page_id_t::make(static_cast<page_space_t>(i), 0);
      disk_page_t header(id, file_header_bytes);
      if (st.st_size == 0) {
        std::fill(header.bytes().begin(), header.bytes().end(), std::byte{0});
        std::memcpy(header.bytes().data(), file_magic, 8);
        put32(header.bytes(), 8, 2); // v2: CSR spaces use an eightfold size progression.
        put32(header.bytes(), 12, static_cast<uint32_t>(i));
        put32(header.bytes(), 16, sizes[i]);
        put32(header.bytes(), 20, file_header_bytes);
        transfer(space.fd, header.bytes().data(), file_header_bytes, 0, true);
      } else {
        transfer(space.fd, header.bytes().data(), file_header_bytes, 0, false);
        if (std::memcmp(header.bytes().data(), file_magic, 8) != 0 ||
            get32(header.bytes(), 8) != 2 || get32(header.bytes(), 12) != i ||
            get32(header.bytes(), 16) != sizes[i] ||
            get32(header.bytes(), 20) != file_header_bytes) {
          throw std::runtime_error("Incompatible page space header or configured size");
        }
        const uint64_t payload = static_cast<uint64_t>(st.st_size) - file_header_bytes;
        const uint64_t count = payload / sizes[i];
        if (payload % sizes[i] != 0 || count > uint64_t(page_id_t::slot_mask) + 1) {
          throw std::runtime_error("Invalid page space length");
        }
        space.count.store(count);
      }
    }
    for (size_t i = 0; i < options.io_workers; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  } catch (...) {
    {
      std::lock_guard lock(queue_mutex_);
      stopping_ = true;
    }
    work_ready_.notify_all();
    for (auto& worker : workers_) worker.join();
    close_files();
    throw;
  }
}

multiscale_disk_manager_t::~multiscale_disk_manager_t() {
  try { shutdown(); } catch (...) { /* Explicit shutdown() reports durability errors. */ }
}

void multiscale_disk_manager_t::check_open() const {
  if (closed_) throw std::runtime_error("Disk manager is closed");
}

uint32_t multiscale_disk_manager_t::page_size(page_space_t space) const {
  return spaces_[space_index(space)].page_size;
}

uint64_t multiscale_disk_manager_t::get_page_count(page_space_t space) const {
  std::shared_lock lock(lifecycle_mutex_);
  check_open();
  return spaces_[space_index(space)].count.load(std::memory_order_acquire);
}

uint64_t multiscale_disk_manager_t::get_sync_count(page_space_t space) const {
  return spaces_[space_index(space)].sync_count.load(std::memory_order_relaxed);
}

// Publish AFTER every write attempt, including partial-transfer failures. A concurrent
// sync can capture an older epoch, but cannot consume a later write's dirty indication.
struct write_epoch_guard_t {
  std::atomic<uint64_t>& epoch;
  ~write_epoch_guard_t() { epoch.fetch_add(1, std::memory_order_release); }
};

page_id_t multiscale_disk_manager_t::allocate_page(page_space_t space_id) {
  return allocate_pages(space_id, 1);
}

page_id_t multiscale_disk_manager_t::allocate_pages(page_space_t space_id, uint32_t count) {
  if (!count) throw std::invalid_argument("Empty extent");
  std::shared_lock lock(lifecycle_mutex_);
  check_open();
  auto& space = spaces_[space_index(space_id)];
  std::lock_guard allocation_lock(space.allocation_mutex);
  const uint64_t slot = space.count.load(std::memory_order_relaxed);
  if (slot + count - 1 > page_id_t::slot_mask) throw std::overflow_error("Page space exhausted");
  const auto id = page_id_t::make(space_id, static_cast<uint32_t>(slot));
  const off_t end = page_offset(id, space.page_size) + uint64_t(count) * space.page_size;
  write_epoch_guard_t changed{space.write_epoch};
  if (::ftruncate(space.fd, end) != 0) io_error("Allocate page slot");
  space.count.store(slot + count, std::memory_order_release);
  return id;
}

void multiscale_disk_manager_t::read_page(page_id_t id,
                                        std::span<std::byte> destination) const {
  std::shared_lock lock(lifecycle_mutex_);
  check_open();
  const auto& space = spaces_[space_index(id.space())];
  if (destination.size() != space.page_size) throw std::invalid_argument("Read buffer size");
  if (id.slot() >= space.count.load(std::memory_order_acquire)) {
    throw std::out_of_range("Reading an unallocated page");
  }
  const auto offset = page_offset(id, space.page_size);
  if (needs_bounce(destination.data(), options_.bypass_cache)) {
    disk_page_t aligned(id, space.page_size);
    transfer(space.fd, aligned.bytes().data(), space.page_size, offset, false);
    std::memcpy(destination.data(), aligned.bytes().data(), space.page_size);
  } else {
    transfer(space.fd, destination.data(), space.page_size, offset, false);
  }
}

disk_page_t multiscale_disk_manager_t::read_page(page_id_t id) const {
  disk_page_t page(id, page_size(id.space()));
  read_page(id, page.bytes());
  return page;
}

void multiscale_disk_manager_t::write_page(page_id_t id, std::span<const std::byte> source) {
  std::shared_lock lock(lifecycle_mutex_);
  check_open();
  auto& space = spaces_[space_index(id.space())];
  if (source.size() != space.page_size) throw std::invalid_argument("Write buffer size");
  if (id.slot() >= space.count.load(std::memory_order_acquire)) {
    throw std::out_of_range("Writing an unallocated page");
  }
  const auto offset = page_offset(id, space.page_size);
  write_epoch_guard_t changed{space.write_epoch};
  if (needs_bounce(source.data(), options_.bypass_cache)) {
    disk_page_t aligned(id, space.page_size);
    std::memcpy(aligned.bytes().data(), source.data(), space.page_size);
    transfer(space.fd, aligned.bytes().data(), space.page_size, offset, true);
  } else {
    // pwrite never mutates the source; transfer shares the read/write loop.
    transfer(space.fd, const_cast<std::byte*>(source.data()), space.page_size, offset, true);
  }
}

void multiscale_disk_manager_t::worker_loop() {
  for (;;) {
    read_job_t job;
    {
      std::unique_lock lock(queue_mutex_);
      work_ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) return;
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    queue_available_.notify_all();
    if (auto* owned = std::get_if<owned_read_job_t>(&job)) {
      (*owned)();
    } else {
      auto& request = std::get<inplace_read_job_t>(job);
      std::exception_ptr error;
      try { read_page(request.id, request.destination); }
      catch (...) { error = std::current_exception(); }
      request.completion(error);
    }
  }
}

std::vector<page_read_request_t> multiscale_disk_manager_t::read_pages_async(
    std::span<const page_id_t> ids) {
  for (auto id : ids) (void)id.space(); // Reject invalid encodings before partial submission.
  std::vector<page_read_request_t> requests;
  requests.reserve(ids.size());
  size_t next = 0;
  std::unique_lock lock(queue_mutex_);
  if (stopping_) throw std::runtime_error("Disk manager is shutting down");
  while (next < ids.size()) {
    queue_available_.wait(lock, [this] {
      return stopping_ || queue_.size() < options_.queue_capacity;
    });
    if (stopping_) throw std::runtime_error("Disk manager is shutting down");
    const size_t count = std::min(ids.size() - next, options_.queue_capacity - queue_.size());
    for (size_t n = 0; n < count; ++n) {
      const auto id = ids[next++];
      owned_read_job_t job([this, id] { return read_page(id); });
      requests.push_back({id, job.get_future()});
      queue_.push_back(std::move(job));
    }
    work_ready_.notify_all();
  }
  return requests;
}

void multiscale_disk_manager_t::read_into_async(
    page_id_t id, std::span<std::byte> destination,
    std::function<void(std::exception_ptr)> completion) {
  if (destination.size() != page_size(id.space()) ||
      reinterpret_cast<uintptr_t>(destination.data()) % 4096 != 0 || !completion) {
    throw std::invalid_argument("Async destination must be aligned and exactly one page");
  }
  read_job_t job(inplace_read_job_t{id, destination, std::move(completion)});
  {
    std::unique_lock lock(queue_mutex_);
    queue_available_.wait(lock, [this] {
      return stopping_ || queue_.size() < options_.queue_capacity;
    });
    if (stopping_) throw std::runtime_error("Disk manager is shutting down");
    queue_.push_back(std::move(job));
  }
  work_ready_.notify_one();
}

page_read_request_t multiscale_disk_manager_t::read_page_async(page_id_t id) {
  auto requests = read_pages_async(std::span<const page_id_t>(&id, 1));
  return std::move(requests.front());
}

std::vector<disk_page_t> multiscale_disk_manager_t::read_pages(std::span<const page_id_t> ids) {
  auto requests = read_pages_async(ids);
  std::vector<disk_page_t> pages;
  pages.reserve(requests.size());
  std::exception_ptr error;
  for (auto& request : requests) {
    try { pages.push_back(request.result.get()); }
    catch (...) { if (!error) error = std::current_exception(); }
  }
  if (error) std::rethrow_exception(error); // Drain all requests before reporting batch failure.
  return pages;
}

void multiscale_disk_manager_t::sync_files() {
  std::exception_ptr error;
  for (auto& space : spaces_) {
    std::lock_guard sync_lock(space.sync_mutex);
    const auto epoch = space.write_epoch.load(std::memory_order_acquire);
    if (epoch == space.synced_epoch) continue;
    int result;
    do {
#ifdef __APPLE__
      result = ::fcntl(space.fd, F_FULLFSYNC);
#else
      result = ::fdatasync(space.fd);
#endif
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
      space.synced_epoch = epoch;
      space.sync_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (result < 0 && !error) {
      error = std::make_exception_ptr(
          std::system_error(errno, std::generic_category(), "Sync page space"));
    }
  }
  if (error) std::rethrow_exception(error);
}

void multiscale_disk_manager_t::flush() {
  std::shared_lock lock(lifecycle_mutex_);
  check_open();
  sync_files();
}

void multiscale_disk_manager_t::close_files() noexcept {
  for (auto& space : spaces_) {
    if (space.fd >= 0) ::close(std::exchange(space.fd, -1));
  }
  closed_ = true;
}

void multiscale_disk_manager_t::shutdown() {
  std::lock_guard shutdown_lock(shutdown_mutex_);
  {
    std::lock_guard lock(queue_mutex_);
    stopping_ = true;
  }
  work_ready_.notify_all();
  queue_available_.notify_all();
  for (auto& worker : workers_) if (worker.joinable()) worker.join();
  std::unique_lock lock(lifecycle_mutex_); // Wait for synchronous callers before closing.
  if (closed_) return;
  std::exception_ptr error;
  try { sync_files(); } catch (...) { error = std::current_exception(); }
  close_files();
  if (error) std::rethrow_exception(error);
}

} // namespace bw_graph
