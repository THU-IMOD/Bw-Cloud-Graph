#include "bw_graph/buf/multiscale_buf_pool.h"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <cstdlib>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>

namespace bw_graph {
namespace multiscale_buf_detail {
enum class frame_state_t { FREE, LOADING, READY, EVICTING };
struct group_t;
// Reserve pins during FREE/LOADING/EVICTING; only READY frames accept CAS pins.
constexpr size_t reserved_pins = std::numeric_limits<size_t>::max();
struct alignas(128) frame_t {
  group_t* group{nullptr};
  chunk_t* chunk{nullptr};
  uint32_t page_size{0};
  std::byte* data{nullptr};
  page_id_t id;
  uint64_t generation{0};
  std::atomic<size_t> pins{reserved_pins};
  std::atomic<uint64_t> hits{0};
  frame_state_t state{frame_state_t::FREE};
  std::atomic<bool> dirty{false}, referenced{false};
  content_latch_t content_mutex;
};
struct chunk_header_t {
  size_t count;
  size_t bytes;
  frame_t* controls;
  std::byte* frames;
};
size_t checked_add(size_t a, size_t b) {
  if (a > std::numeric_limits<size_t>::max() - b) throw std::overflow_error("Chunk size overflow");
  return a + b;
}
size_t checked_mul(size_t a, size_t b) {
  if (b && a > std::numeric_limits<size_t>::max() / b) {
    throw std::overflow_error("Chunk size overflow");
  }
  return a * b;
}
size_t align_up(size_t value, size_t alignment) {
  return checked_add(value, alignment - 1) / alignment * alignment;
}
struct chunk_t : std::enable_shared_from_this<chunk_t> {
  void* memory{nullptr};
  chunk_header_t* header{nullptr};
  size_t constructed{0};
  chunk_t(size_t count, uint32_t size, group_t* group) {
    const size_t control_offset = align_up(sizeof(chunk_header_t), alignof(frame_t));
    const size_t data_offset = align_up(checked_add(control_offset,
                                                  checked_mul(count, sizeof(frame_t))), 4096);
    const size_t bytes = checked_add(data_offset, checked_mul(count, size));
    if (::posix_memalign(&memory, 4096, bytes) != 0) throw std::bad_alloc();
    auto* base = static_cast<std::byte*>(memory);
    auto* controls = reinterpret_cast<frame_t*>(base + control_offset);
    header = std::construct_at(static_cast<chunk_header_t*>(memory),
                              chunk_header_t{count, bytes, controls, base + data_offset});
    try {
      for (; constructed < count; ++constructed) {
        auto* f = std::construct_at(controls + constructed);
        f->group = group;
        f->chunk = this;
        f->page_size = size;
        f->data = header->frames + constructed * size;
      }
    } catch (...) { destroy(); throw; }
  }
  void destroy() noexcept {
    for (size_t i = 0; i < constructed; ++i) std::destroy_at(header->controls + i);
    if (header) std::destroy_at(header);
    std::free(memory);
  }
  ~chunk_t() { destroy(); }
};
struct load_t {
  page_id_t id;
  frame_t* frame{nullptr};
  page_id_t old_id;
  uint64_t generation{0};
  bool synchronous_owner{false};
  bool initialize{false};
  bool dirty_victim{false}, old_written{false}, old_removed{false}, bound{false};
  std::vector<std::shared_ptr<std::promise<page_pin_t>>> waiters;
};
struct group_t {
  uint32_t page_size{0};
  std::shared_mutex mutex;
  std::condition_variable_any changed;
  std::vector<std::shared_ptr<chunk_t>> chunks;
  std::vector<frame_t*> frames, free;
  folly::ConcurrentHashMap<uint32_t, frame_t*> pages;
  std::unordered_map<uint32_t, std::shared_ptr<load_t>> loading;
  size_t hand{0};
  buffer_group_stats_t counters;
};

struct state_t : std::enable_shared_from_this<state_t> {
  multiscale_disk_manager_t& disk;
  std::array<group_t, 6> groups;
  std::atomic<bool> closing{false};
  std::mutex close_mutex;
  bool closed{false};

  state_t(multiscale_disk_manager_t& d, const multiscale_buf_options_t& options) : disk(d) {
    // Validate all six budgets before allocating any chunks.
    size_t total = 0;
    for (size_t i = 0; i < groups.size(); ++i) {
      const auto cfg = options.groups[i];
      if (cfg.chunk_count && !cfg.chunk_size) throw std::invalid_argument("Empty enabled chunk");
      auto& g = groups[i];
      g.page_size = disk.page_size(static_cast<page_space_t>(i));
      g.counters.frame_count = checked_mul(cfg.chunk_count, cfg.chunk_size);
      if (!cfg.chunk_count) continue;
      const auto control_end = checked_add(align_up(sizeof(chunk_header_t), alignof(frame_t)),
                                           checked_mul(cfg.chunk_size, sizeof(frame_t)));
      const auto bytes = checked_add(align_up(control_end, 4096),
                                     checked_mul(cfg.chunk_size, g.page_size));
      g.counters.chunk_bytes = checked_mul(cfg.chunk_count, bytes);
      total = checked_add(total, g.counters.chunk_bytes);
    }
    for (size_t i = 0; i < groups.size(); ++i) {
      auto& g = groups[i];
      const auto cfg = options.groups[i];
      g.frames.reserve(g.counters.frame_count);
      g.free.reserve(g.counters.frame_count);
      g.pages = folly::ConcurrentHashMap<uint32_t, frame_t*>(
          std::max<size_t>(8, g.counters.frame_count));
      g.loading.reserve(g.counters.frame_count);
      g.chunks.reserve(cfg.chunk_count);
      for (size_t c = 0; c < cfg.chunk_count; ++c) {
        auto chunk = std::make_shared<chunk_t>(cfg.chunk_size, g.page_size, &g);
        g.chunks.push_back(std::move(chunk));
      }
      // Adjacent logical allocations spread across chunks: concurrent neighboring
      // accesses do not all update one chunk's lifetime reference counter.
      for (size_t f = 0; f < cfg.chunk_size && cfg.chunk_count; ++f) {
        for (auto& chunk : g.chunks) {
          g.frames.push_back(chunk->header->controls + f);
        }
      }
      // Pop oldest allocation order first, matching the initial CLOCK hand.
      g.free.assign(g.frames.rbegin(), g.frames.rend());
    }
  }
  group_t& group(page_id_t id) { return groups[static_cast<size_t>(id.space())]; }
  void check_open() const {
    if (closing.load(std::memory_order_acquire)) throw std::runtime_error("Buffer pool closing");
  }
  static page_pin_t handle(frame_t* f) {
    return page_pin_t(f->chunk->shared_from_this(), f);
  }
  frame_t* choose(group_t& g) {
    if (!g.free.empty()) {
      auto* f = g.free.back();
      g.free.pop_back();
      return f;
    }
    // Two CLOCK revolutions give unpinned referenced pages a second chance.
    for (size_t pass = 0; pass < 2; ++pass) {
      for (size_t n = 0; n < g.frames.size(); ++n) {
        auto* f = g.frames[g.hand];
        g.hand = (g.hand + 1) % g.frames.size();
        if (f->state != frame_state_t::READY || f->pins.load(std::memory_order_acquire) != 0)
          continue;
        if (f->referenced.exchange(false, std::memory_order_relaxed)) continue;
        size_t expected = 0;
        if (f->pins.compare_exchange_strong(expected, reserved_pins,
                                            std::memory_order_acq_rel)) return f;
      }
    }
    throw buffer_full_t();
  }
  page_pin_t pin_frame(frame_t* f, page_id_t id) {
    if (!f) return {};
    size_t pins = f->pins.load(std::memory_order_relaxed);
    while (pins != reserved_pins) {
      if (f->pins.compare_exchange_weak(pins, pins + 1, std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
        if (f->id == id && !closing.load(std::memory_order_acquire)) {
          f->referenced.store(true, std::memory_order_relaxed);
          f->hits.fetch_add(1, std::memory_order_relaxed);
          return handle(f);
        }
        f->pins.fetch_sub(1, std::memory_order_release);
        break;
      }
    }
    return {};
  }
  page_pin_t try_pin(page_id_t id) {
    auto& g = group(id);
    check_open();
    auto it = g.pages.find(id.raw());
    return it == g.pages.end() ? page_pin_t{} : pin_frame(it->second, id);
  }
  page_pin_t pin(page_id_t id) {
    auto cached = try_pin(id);
    if (cached.frame_) return cached;
    page_pin_t result;
    auto waiting = fetch(id, false, &result);
    if (waiting.valid()) return waiting.get(); // Only a coalesced synchronous miss needs a future.
    return result;
  }
  // No allocations on completion: mappings/free-list capacity were reserved beforehand.
  // Fulfill promises OUTSIDE the group lock: discarded futures can immediately unpin.
  void finish(const std::shared_ptr<load_t>& load, std::exception_ptr error) noexcept {
    auto& g = group(load->id);
    auto* f = load->frame;
    std::vector<std::shared_ptr<std::promise<page_pin_t>>> waiters;
    {
      std::lock_guard lock(g.mutex);
      assert(f && f->pins == reserved_pins);
      if (!error) {
        assert(load->bound && f->id == load->id && f->generation == load->generation);
        f->state = frame_state_t::READY;
        // Fresh output has not demonstrated reuse. Admit it cold so bulk SMO
        // output cannot give every streaming page a second chance ahead of
        // already useful base/Delta pages. Any subsequent hit promotes it.
        f->referenced = !(load->initialize && static_cast<unsigned>(load->id.space()) < 4);
        f->pins.store(load->waiters.size() + size_t(load->synchronous_owner),
                      std::memory_order_release);
        if (!load->initialize) ++g.counters.disk_reads;
      } else {
        ++g.counters.failures;
        if (load->bound) g.pages.erase(load->id.raw());
        if (load->old_id.valid() && !load->old_removed) {
          f->state = frame_state_t::READY;
          f->dirty = load->dirty_victim && !load->old_written;
          f->pins.store(0, std::memory_order_release);
        } else {
          f->id = page_id_t{};
          f->state = frame_state_t::FREE;
          f->dirty = false;
          g.free.push_back(f);
        }
      }
      waiters.swap(load->waiters);
      g.loading.erase(load->id.raw());
    }
    g.changed.notify_all();
    for (auto& promise : waiters) {
      if (error) promise->set_exception(error);
      else promise->set_value(handle(f));
    }
  }
  std::future<page_pin_t> fetch(page_id_t id, bool asynchronous = true,
                                page_pin_t* direct = nullptr, bool initialize = false) {
    std::shared_ptr<std::promise<page_pin_t>> promise;
    std::future<page_pin_t> future;
    auto make_promise = [&] {
      promise = std::make_shared<std::promise<page_pin_t>>();
      future = promise->get_future();
    };
    if (!direct) make_promise();
    std::shared_ptr<load_t> load;
    try {
      auto cached = try_pin(id);
      if (cached.frame_) {
        if (initialize) throw std::logic_error("new_page ID is already cached");
        if (direct) *direct = std::move(cached);
        else promise->set_value(std::move(cached));
        return future;
      }
      auto& g = group(id);
      {
        std::unique_lock lock(g.mutex);
        for (;;) {
          check_open();
          auto in_flight = g.loading.find(id.raw());
          if (in_flight != g.loading.end()) {
            if (initialize) throw std::logic_error("new_page ID is already loading");
            if (!promise) make_promise();
            in_flight->second->waiters.push_back(promise);
            ++g.counters.coalesced;
            return future;
          }
          auto it = g.pages.find(id.raw());
          if (it == g.pages.end()) break;
          auto* f = it->second;
          if (initialize) throw std::logic_error("new_page ID is already mapped");
          if (f->state == frame_state_t::READY) {
            ++f->pins;
            f->referenced = true;
            f->hits.fetch_add(1, std::memory_order_relaxed);
            lock.unlock();
            if (direct) *direct = handle(f);
            else promise->set_value(handle(f));
            return future;
          }
          // Old page remains mapped while dirty eviction writes it back.
          g.changed.wait(lock);
        }
        if (g.frames.empty()) throw std::runtime_error("Page space buffer is disabled");
        load = std::make_shared<load_t>();
        load->id = id;
        load->initialize = initialize;
        load->synchronous_owner = direct != nullptr;
        if (promise) load->waiters.push_back(promise);
        g.loading.emplace(id.raw(), load);
        try { load->frame = choose(g); }
        catch (...) { g.loading.erase(id.raw()); throw; }
        auto* f = load->frame;
        load->old_id = f->id;
        load->dirty_victim = f->dirty;
        assert(f->pins == reserved_pins); // Covers eviction, admission and completion.
        f->state = frame_state_t::EVICTING;
        ++g.counters.misses;
      }
      auto* f = load->frame;
      // Exclusive reservation prevents content access, so no extra content latch is needed.
      if (load->dirty_victim) {
        disk.write_page(load->old_id, {f->data, g.page_size});
        load->old_written = true;
      }
      {
        std::lock_guard lock(g.mutex);
        if (load->old_written) ++g.counters.writebacks;
        if (load->old_id.valid()) {
          g.pages.erase(load->old_id.raw());
          load->old_removed = true;
          ++g.counters.evictions;
        }
        g.pages.insert(id.raw(), f); // If allocation fails, finish() restores/frees reservation.
        load->bound = true;
        f->id = id;
        load->generation = ++f->generation;
        f->state = frame_state_t::LOADING;
        f->dirty = false;
      }
      g.changed.notify_all();
      if (asynchronous) {
        disk.read_into_async(id, {f->data, g.page_size},
            [self = shared_from_this(), load](std::exception_ptr error) noexcept {
              self->finish(load, error);
            });
      } else {
        if (initialize) std::memset(f->data, 0, g.page_size);
        else disk.read_page(id, {f->data, g.page_size});
        finish(load, nullptr);
        *direct = handle(f);
      }
    } catch (...) {
      auto error = std::current_exception();
      if (load && load->frame) finish(load, error);
      else if (promise) promise->set_exception(error);
      if (direct) std::rethrow_exception(error);
    }
    return future;
  }
  void flush_frame(frame_t* f) {
    auto& g = *f->group;
    // Caller holds an internal pin. Never wait for content while holding metadata mutex.
    std::unique_lock content_lock(f->content_mutex);
    bool dirty;
    { std::lock_guard lock(g.mutex); dirty = f->dirty; }
    if (dirty) {
      disk.write_page(f->id, {f->data, g.page_size});
      std::lock_guard lock(g.mutex);
      f->dirty = false;
      ++g.counters.writebacks;
    }
  }
  void flush_page(page_id_t id) {
    auto& g = group(id);
    frame_t* f = nullptr;
    {
      std::unique_lock lock(g.mutex);
      for (;;) {
        auto it = g.pages.find(id.raw());
        if (it == g.pages.end()) {
          if (g.loading.count(id.raw())) { g.changed.wait(lock); continue; }
          return;
        }
        if (it->second->state != frame_state_t::READY) { g.changed.wait(lock); continue; }
        f = it->second;
        ++f->pins;
        break;
      }
    }
    auto pin = handle(f);
    flush_frame(f);
  }
  void flush_all() {
    // Pin one frame at a time; no giant snapshot of pinned pages and no lock over disk I/O.
    for (auto& g : groups) {
      for (auto* f : g.frames) {
        {
          std::unique_lock lock(g.mutex);
          g.changed.wait(lock, [f] {
            return f->state == frame_state_t::FREE || f->state == frame_state_t::READY;
          });
          if (f->state == frame_state_t::FREE) continue;
          ++f->pins;
        }
        auto pin = handle(f);
        flush_frame(f);
      }
    }
    disk.flush();
  }
  bool close() {
    std::lock_guard close_lock(close_mutex);
    if (closed) return true;
    closing.store(true, std::memory_order_release);
    for (auto& g : groups) {
      g.changed.notify_all();
      std::unique_lock lock(g.mutex);
      g.changed.wait(lock, [&] { return g.loading.empty(); });
    }
    for (auto& g : groups) {
      std::lock_guard lock(g.mutex);
      for (auto* f : g.frames)
        if (f->state == frame_state_t::READY &&
            f->pins.load(std::memory_order_acquire) != 0) return false;
    }
    flush_all();
    closed = true;
    return true;
  }
};
} // namespace multiscale_buf_detail

using multiscale_buf_detail::frame_t;
using multiscale_buf_detail::chunk_t;
using multiscale_buf_detail::state_t;
page_pin_t::page_pin_t(std::shared_ptr<chunk_t> chunk, frame_t* frame)
    : chunk_(std::move(chunk)), frame_(frame) {}
page_pin_t::~page_pin_t() { reset(); }
void page_pin_t::reset() noexcept {
  if (frame_) {
    // This also works after the pool wrapper/groups have been destroyed: the pin
    // owns its chunk and unpin needs no map, group, or disk-manager access.
    assert(frame_->pins.load(std::memory_order_relaxed) > 0);
    if (dirty_) frame_->dirty.store(true, std::memory_order_relaxed);
    frame_->pins.fetch_sub(1, std::memory_order_release);
  }
  frame_ = nullptr;
  chunk_.reset();
  dirty_ = false;
}
page_pin_t::page_pin_t(page_pin_t&& other) noexcept
    : chunk_(std::move(other.chunk_)), frame_(std::exchange(other.frame_, nullptr)),
      dirty_(std::exchange(other.dirty_, false)) {}
page_pin_t& page_pin_t::operator=(page_pin_t&& other) noexcept {
  if (this != &other) {
    reset();
    chunk_ = std::move(other.chunk_);
    frame_ = std::exchange(other.frame_, nullptr);
    dirty_ = std::exchange(other.dirty_, false);
  }
  return *this;
}
page_id_t page_pin_t::page_id() const {
  if (!frame_) throw std::logic_error("Empty page pin");
  return frame_->id;
}
read_page_guard_t page_pin_t::read() && { return read_page_guard_t(std::move(*this)); }
write_page_guard_t page_pin_t::write() && { return write_page_guard_t(std::move(*this)); }
read_page_guard_t::read_page_guard_t(page_pin_t pin) : pin_(std::move(pin)) {
  (void)pin_.page_id();
  lock_ = std::shared_lock(pin_.frame_->content_mutex);
}
std::span<const std::byte> read_page_guard_t::bytes() const {
  (void)pin_.page_id();
  return {pin_.frame_->data, pin_.frame_->page_size};
}
write_page_guard_t::write_page_guard_t(page_pin_t pin) : pin_(std::move(pin)) {
  (void)pin_.page_id();
  lock_ = std::unique_lock(pin_.frame_->content_mutex);
  pin_.dirty_ = true;
}
std::span<std::byte> write_page_guard_t::bytes() {
  (void)pin_.page_id();
  return {pin_.frame_->data, pin_.frame_->page_size};
}
multiscale_buf_pool_t::multiscale_buf_pool_t(multiscale_disk_manager_t& disk,
                                           multiscale_buf_options_t options)
    : state_(std::make_shared<state_t>(disk, options)) {}
multiscale_buf_pool_t::~multiscale_buf_pool_t() {
  try { (void)state_->close(); } catch (...) { /* Explicit close reports durability errors. */ }
}
page_pin_t multiscale_buf_pool_t::pin_page(page_id_t id) { return state_->pin(id); }
page_pin_t multiscale_buf_pool_t::try_pin_page(page_id_t id) { return state_->try_pin(id); }
read_page_guard_t multiscale_buf_pool_t::fetch_read(page_id_t id) {
  return state_->pin(id).read();
}
read_page_guard_t multiscale_buf_pool_t::fetch_read(page_id_t id, page_frame_hint_t& hint) {
  state_->check_open();
  auto pin = state_->pin_frame(hint.frame.load(std::memory_order_relaxed), id);
  if (!pin.frame_) {
    pin = state_->pin(id);
    hint.frame.store(pin.frame_, std::memory_order_relaxed);
  }
  return std::move(pin).read();
}
write_page_guard_t multiscale_buf_pool_t::fetch_write(page_id_t id) {
  return state_->pin(id).write();
}
write_page_guard_t multiscale_buf_pool_t::fetch_write(page_id_t id, page_frame_hint_t& hint) {
  state_->check_open();
  auto pin = state_->pin_frame(hint.frame.load(std::memory_order_relaxed), id);
  if (!pin.frame_) {
    pin = state_->pin(id);
    hint.frame.store(pin.frame_, std::memory_order_relaxed);
  }
  return std::move(pin).write();
}
write_page_guard_t multiscale_buf_pool_t::new_page(page_id_t id) {
  if (id.slot() >= state_->disk.get_page_count(id.space()))
    throw std::invalid_argument("new_page requires an allocated disk ID");
  page_pin_t result;
  (void)state_->fetch(id, false, &result, true);
  return std::move(result).write();
}
buffer_read_request_t multiscale_buf_pool_t::fetch_async(page_id_t id) {
  return {id, state_->fetch(id)};
}
std::vector<buffer_read_request_t> multiscale_buf_pool_t::fetch_batch(
    std::span<const page_id_t> ids) {
  std::vector<buffer_read_request_t> requests;
  requests.reserve(ids.size());
  for (auto id : ids) requests.push_back(fetch_async(id));
  return requests;
}
void multiscale_buf_pool_t::prefetch(page_id_t id) {
  try {
    state_->check_open();
    auto& g = state_->group(id);
    // A hint for an already resident/loading page needs no future, pin, or latch.
    if (g.pages.find(id.raw()) == g.pages.end()) (void)state_->fetch(id);
  } catch (...) { /* Best effort: demand reads report errors. */ }
}
void multiscale_buf_pool_t::flush_page(page_id_t id) { state_->flush_page(id); }
void multiscale_buf_pool_t::flush_all() { state_->flush_all(); }
bool multiscale_buf_pool_t::close() { return state_->close(); }
buffer_group_stats_t multiscale_buf_pool_t::stats(page_space_t space) const {
  const auto i = static_cast<size_t>(space);
  if (i >= 6) throw std::invalid_argument("Invalid page space");
  auto& g = state_->groups[i];
  std::lock_guard lock(g.mutex);
  auto result = g.counters;
  for (auto* f : g.frames) {
    result.hits += f->hits.load(std::memory_order_relaxed);
    const auto pins = f->pins.load(std::memory_order_relaxed);
    if (f->state != multiscale_buf_detail::frame_state_t::FREE && pins) ++result.pinned_frames;
  }
  return result;
}
} // namespace bw_graph
