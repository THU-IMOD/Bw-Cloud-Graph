#ifndef BW_GRAPH_DB_MULTISCALE_STORE_H
#define BW_GRAPH_DB_MULTISCALE_STORE_H

#include "bw_graph/buf/multiscale_buf_pool.h"
#include "bw_graph/common/type.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/io_delta.h"
#include <atomic>
#include "bw_graph/common/direction.h"
#include <shared_mutex>
#include <cstring>
#include <iterator>
#include <optional>
#include <condition_variable>
#include <deque>
#include <thread>
#include <functional>
#include <map>
#include <unordered_map>
#include "bw_graph/db/txn.h"
#include "bw_graph/index/multiscale_tat.h"
#include <type_traits>
#include <tbb/concurrent_vector.h>
#include "bw_graph/index/vertex_table.h"

namespace bw_graph {
struct neighbor_fragment_t {
  page_id_t page;
  uint32_t byte_offset{0};
  uint32_t count{0}; // Contiguous pages.
  uint64_t edges{0}; // Valid edges in this extent; zero denotes the initial packed layout.
};
struct directed_delta_record_t : delta_record_t { direction_t direction{direction_t::out}; };
struct adjacency_t {
  std::vector<neighbor_fragment_t> rest;
  uint64_t degree{0};
  std::atomic<uint64_t> head{UINT64_MAX};
  std::atomic<uint64_t> in_head{UINT64_MAX};
  std::shared_ptr<adjacency_t> incoming;
  neighbor_fragment_t first;
  std::atomic<bool> deleted{false};
  std::atomic<bool> unattached{false};
  page_id_t anchor;
  page_id_t leaf; // Current CSR slot page; Delta ownership remains at anchor.
  uint64_t stop{UINT64_MAX};
  uint64_t base_ts{0};
  // Published base generations are immutable. Borrowed ranges own their generation.
  std::shared_ptr<adjacency_t> version;
  mutable std::atomic<uint64_t> reads{0};
};
inline const adjacency_t& incoming_base(const adjacency_t& a) {
  static const adjacency_t empty;
  return a.incoming ? *a.incoming : empty;
}
using vertex_directory_t = vertex_table_t<adjacency_t>;
class multiscale_store_t;

// Single-pass, borrowed range. DB must outlive it; no edge array is allocated/copied.
// Published base generations are immutable, including across SMO. One frame is pinned at a time.
class neighbor_range_t {
public:
  neighbor_range_t() = default;
  neighbor_range_t(neighbor_span_t span, csr_page_t* page);
  neighbor_range_t(multiscale_store_t& store, const adjacency_t& adjacency);
  ~neighbor_range_t();
  neighbor_range_t(neighbor_range_t&&) noexcept;
  neighbor_range_t(const neighbor_range_t&) = delete;
  neighbor_range_t& operator=(neighbor_range_t&&) = delete;
  struct iterator {
    using value_type = v_id_t;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;
    neighbor_range_t* range{nullptr};
    v_id_t operator*() const {
      v_id_t value;
      std::memcpy(&value, range->data_ + range->position_ * 4, 4);
      return value;
    }
    iterator& operator++() {
      ++range->position_;
      --range->remaining_;
      if (range->remaining_ && range->position_ == range->count_) range->load_next();
      return *this;
    }
    void operator++(int) { ++*this; }
    bool operator==(std::default_sentinel_t) const { return !range->remaining_; }
  };
  iterator begin() { return {this}; }
  std::default_sentinel_t end() const { return {}; }
  uint64_t delta_head() const { return head_; }
  uint64_t page_count(page_space_t space) const {
    if (!adjacency_) return 0;
    uint64_t count = adjacency_->first.page.valid() && adjacency_->first.page.space() == space ?
        adjacency_->first.count : 0;
    for (const auto& f : adjacency_->rest) if (f.page.space() == space) count += f.count;
    return count;
  }
  uint64_t base_timestamp() const { return adjacency_ ? adjacency_->base_ts : 0; }
  uint64_t delta_stop() const { return adjacency_ ? adjacency_->stop : UINT64_MAX; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  // Consume page-local byte spans without copying adjacency. The callback returns
  // true to stop. Spans may only be used inside the callback (including joined
  // parallel work); advancing releases the previous page guard.
  template <class F> void visit_chunks(F&& f) {
    while (remaining_) {
      const size_t n = std::min(remaining_, count_ - position_);
      if (f(std::span<const std::byte>(data_ + position_ * 4, n * 4))) return;
      remaining_ -= n;
      position_ += n;
      if (remaining_) load_next();
    }
  }
private:
  friend class multiscale_store_t;
  explicit neighbor_range_t(std::span<const std::byte> bytes)
      : data_(bytes.data()), count_(bytes.size() / 4), remaining_(count_), size_(count_) {}
  void load_next();
  multiscale_store_t* store_{nullptr};
  const adjacency_t* adjacency_{nullptr};
  std::shared_ptr<adjacency_t> version_;
  uint64_t head_{UINT64_MAX};
  uint64_t extent_consumed_{0};
  csr_page_t* legacy_page_{nullptr};
  std::optional<read_page_guard_t> guard_;
  const std::byte* data_{nullptr};
  size_t position_{0}, count_{0}, remaining_{0}, size_{0};
  size_t extent_{0};
  uint32_t page_in_extent_{0};
};

// Index pages are resident metadata; only changed images are written at checkpoint.
// SMO publishes immutable base generations. Snapshot transactions defer records newer
// than the oldest reader. Physical allocation remains append-only (no page reuse yet).
class multiscale_store_t {
public:
  multiscale_store_t(const std::filesystem::path& path, bool rebuild, bool id_order,
                    const std::string& graph_file, multiscale_buf_options_t options,
                    size_t smo_workers = 0, size_t delta_page_threshold = 2, bool create_empty = false);
  ~multiscale_store_t();
  neighbor_range_t read_neighbor(v_id_t v, direction_t direction = direction_t::out);
  uint64_t delta_head(v_id_t v) const;
  directed_delta_record_t read_delta(uint64_t pointer);
  bool update(v_id_t src, v_id_t dst, delta_type_t type);
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  uint64_t begin_snapshot();
  void end_snapshot(uint64_t ts);
  bool commit_edges(uint64_t read_ts, const std::vector<write_opt_t>& writes);
  uint64_t watermark();
#endif
  std::vector<v_id_t> clone_at(v_id_t v, uint64_t ts, direction_t direction = direction_t::out);
  std::vector<v_id_t> clone(v_id_t v, direction_t direction = direction_t::out); // Explicit compatibility/materialization API only.
  uint32_t vertex_count() const { return published_vertices_.load(std::memory_order_acquire); }
  bool insert_vertex(v_id_t v);
  v_id_t append_vertex();
  v_id_t append_vertex(std::span<const v_id_t> neighbors);
  bool base_fits() const;
  std::span<const v_id_t> scan_order() const { return scan_order_; }
  std::span<const size_t> scan_groups() const { return scan_groups_; }
  bool delete_vertex(v_id_t v);
  uint64_t base_edges() const { return base_edges_; }
  uint64_t degree(v_id_t v);
  // Base-only cardinality for algorithms. Unlike degree(), never clones edges
  // or traverses Delta records. Matches read_neighbor() visibility.
  uint64_t base_degree(v_id_t v, direction_t direction = direction_t::out) const {
    const auto& root = location(v);
    if (root.deleted.load(std::memory_order_acquire)) return 0;
    auto version = std::atomic_load_explicit(&root.version, std::memory_order_acquire);
    const auto& base = version ? *version : root;
    return direction == direction_t::out ? base.degree : incoming_base(base).degree;
  }
  const adjacency_t& location(v_id_t v) const;
  // Callback ranges borrow this call's page guard and must not escape the callback.
  // Callers group vertices by first page; no base neighbor array is copied.
  template <class F> void visit_base_group(std::span<const v_id_t> vertices, F&& visit) {
    if (vertices.empty()) return;
    if (smo_enabled_) {
      for (auto v : vertices) visit(v, read_neighbor(v));
      return;
    }
    const auto& first = location(vertices.front());
    if (first.first.count > 1 || !first.rest.empty()) {
      for (auto v : vertices) visit(v, read_neighbor(v));
      return;
    }
    const auto id = first.first.page;
    if (!id.valid()) return; // Empty base adjacency.
    const auto c = static_cast<size_t>(id.space());
    std::optional<read_page_guard_t> guard;
    const std::byte* bytes = resident_[c][id.slot()];
    if (!bytes) {
      guard.emplace(pool_->fetch_read(id, hints_[c][id.slot()]));
      validate_base(id, guard->bytes());
      bytes = guard->bytes().data();
    }
    const size_t size = disk_.page_size(id.space());
    for (auto v : vertices) {
      const auto& a = location(v);
      if (a.deleted.load(std::memory_order_acquire)) continue;
      if (a.first.page == id && a.first.count == 1 && a.rest.empty()) {
        if (a.first.byte_offset > size || a.degree > (size - a.first.byte_offset) / 4)
          throw std::runtime_error("Invalid grouped adjacency");
        visit(v, neighbor_range_t(std::span<const std::byte>(bytes + a.first.byte_offset,
                                                           a.degree * 4)));
      } else {
        visit(v, read_neighbor(v));
      }
    }
  }
  void warm_up(bool require_all_giants = true);
  void consolidate_all(); // Requires quiescent producers; includes deferred giants.
  void wait_for_smo();
  struct smo_stats_t { uint64_t jobs, vertices, records, deferred, bytes_written, background_jobs; };
  smo_stats_t smo_stats() const;
  void checkpoint(); // Caller quiesces writers; data synced before changed index pages.
  void close();
  multiscale_disk_manager_t& disk() { return disk_; }
  multiscale_buf_pool_t& pool() { return *pool_; }
  const multiscale_tat_t& tat() const { return tat_; }
  uint64_t index_bytes() const;
  vertex_index_backend_t vertex_index_backend() const { return vertices_.backend(); }
  uint64_t giant_bytes() const;
  const multiscale_buf_options_t& options() const { return options_; }
private:
  friend class neighbor_range_t;
  static void validate_base(page_id_t id, std::span<const std::byte> bytes);
  static directed_delta_record_t decode_delta(uint64_t pointer, std::span<const std::byte> bytes);
  std::vector<v_id_t> topology_edges(v_id_t v);
  void build(const std::string& graph_file, bool id_order);
  void load_index();
  std::vector<std::byte> encode_index() const;
  multiscale_disk_manager_t disk_;
  multiscale_buf_options_t options_;
  std::unique_ptr<multiscale_buf_pool_t> pool_;
  vertex_directory_t vertices_;
  multiscale_tat_t tat_;
  struct delta_writer_t {
    std::mutex mutex;
    page_id_t page;
    page_frame_hint_t hint; // Identity is revalidated after pinning; holds no frame pin.
    uint32_t used{0};
    page_id_t chain;
    std::vector<v_id_t> members;
    size_t pending{0};
    bool queued{false};
  };
  tbb::concurrent_vector<std::unique_ptr<delta_writer_t>> writers_;
  delta_writer_t& writer(v_id_t v) { return *writers_[vertices_[v].anchor.slot()]; }
  void initialize_writers();
  void refresh_scan_order(); // Quiescent algorithm/warmup boundary.
  struct page_reservation_t {
    std::mutex mutex;
    uint32_t next{0}, remaining{0};
  };
  std::array<page_reservation_t, 6> reservations_;
  page_id_t allocate_runtime_page(page_space_t space);
  void queue_smo(uint32_t anchor, delta_writer_t& w);
  void consolidate(std::span<const uint32_t> anchors, bool force);
  void worker_loop();
  void write_base(v_id_t v, std::span<const v_id_t> edges, adjacency_t& out,
                  unsigned max_scale = 3, direction_t direction = direction_t::out);
  bool update_record(v_id_t src, v_id_t dst, delta_type_t type, uint64_t timestamp = 0,
                     uint64_t begin_timestamp = 0, direction_t direction = direction_t::out);
  bool update_pair(v_id_t src, v_id_t dst, delta_type_t type, uint64_t timestamp = 0,
                   uint64_t begin_timestamp = 0);
  std::atomic<uint64_t> committed_ts_{0};
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  std::mutex transaction_mutex_;
  std::map<uint64_t, size_t> snapshots_;
  std::unordered_map<uint64_t, uint64_t> edge_commits_;
#endif
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<uint32_t> jobs_;
  std::vector<std::thread> workers_;
  std::exception_ptr worker_error_;
  size_t active_jobs_{0}, delta_threshold_{2};
  bool stopping_{false}, smo_enabled_{false};
  std::atomic<uint64_t> smo_jobs_{0}, smo_vertices_{0}, smo_records_{0},
      smo_deferred_{0}, smo_bytes_{0}, smo_background_{0};
  std::vector<disk_page_t> index_images_;
  std::array<std::unique_ptr<page_frame_hint_t[]>, 4> hints_;
  std::array<std::vector<const std::byte*>, 4> resident_;
  std::vector<page_pin_t> resident_pins_;
  std::vector<v_id_t> scan_order_;
  std::vector<size_t> scan_groups_;
  mutable std::shared_mutex topology_mutex_;
  page_id_t orphan_page_;
  v_id_t append_vertex_locked(bool deleted = false);
  void attach_locked(v_id_t v, std::span<const v_id_t> neighbors);
  void rewrite_members_locked(std::vector<page_id_t> old,
                              std::vector<std::vector<v_id_t>> groups);
  std::mutex vertex_insert_mutex_;
  std::atomic<uint32_t> published_vertices_{0};
  uint64_t base_edges_{0};
  std::atomic<bool> index_dirty_{false};
  bool closed_{false};
};
} // namespace bw_graph
#endif
