#ifndef BW_GRAPH_INDEX_VERTEX_TABLE_H
#define BW_GRAPH_INDEX_VERTEX_TABLE_H

#include "bw_graph/common/vertex_index_backend.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <tbb/concurrent_vector.h>

namespace bw_graph {
// ART holds stable controls, never the replaceable version itself. Structural
// insertion is serialized; payload/version synchronization belongs to the caller.
struct art_vertex_leaf_t {
  uint64_t key;
  void* value;
};
class art_vertex_map_t {
public:
  art_vertex_map_t();
  ~art_vertex_map_t();
  void insert(art_vertex_leaf_t* leaf);
  void* find(uint64_t key) const;
private:
  struct impl_t;
  std::unique_ptr<impl_t> impl_;
};

// Dense public IDs remain the graph API contract. ART eliminates relocations and
// reserved payload capacity; deleted IDs retain their controls until DB teardown.
// clear/destruction and iteration/checkpoint require quiescent users.
template <class T> class vertex_table_t {
  static constexpr size_t chunk_size = 256;
  using chunk_t = std::array<T, chunk_size>;
  struct entry_t {
    art_vertex_leaf_t leaf;
    T value;
    entry_t* next;
    template <class... Args>
    entry_t(size_t id, entry_t* next, Args&&... args)
        : leaf{id, &value}, value(std::forward<Args>(args)...), next(next) {}
  };
  vertex_index_backend_t backend_;
  tbb::concurrent_vector<std::unique_ptr<chunk_t>> chunks_;
  std::unique_ptr<art_vertex_map_t> art_;
  entry_t* entries_{nullptr};
  std::atomic<size_t> size_{0};
  mutable std::mutex growth_mutex_;

  template <class... Args> T& append_unlocked(Args&&... args) {
    const size_t id = size_.load(std::memory_order_relaxed);
    if (id >= UINT32_MAX) throw std::overflow_error("Vertex ID exhausted");
    T* result;
    if (backend_ == vertex_index_backend_t::art) {
      auto entry = std::make_unique<entry_t>(id, entries_, std::forward<Args>(args)...);
      art_->insert(&entry->leaf);
      result = &entry->value;
      entries_ = entry.release();
    } else {
      const size_t chunk = id / chunk_size;
      chunks_.grow_to_at_least(chunk + 1);
      if (!chunks_[chunk]) chunks_[chunk] = std::make_unique<chunk_t>();
      result = &(*chunks_[chunk])[id % chunk_size];
      if constexpr (sizeof...(Args) > 0) *result = T(std::forward<Args>(args)...);
    }
    size_.store(id + 1, std::memory_order_release);
    return *result;
  }
  template <bool Const> struct iterator_t {
    using table_t = std::conditional_t<Const, const vertex_table_t, vertex_table_t>;
    table_t* table;
    size_t index;
    auto& operator*() const { return (*table)[index]; }
    iterator_t& operator++() { ++index; return *this; }
    bool operator!=(const iterator_t& other) const { return index != other.index; }
  };
public:
  explicit vertex_table_t(size_t count = 0,
                         vertex_index_backend_t backend = BW_VERTEX_INDEX_BACKEND)
      : backend_(backend) {
    if (backend_ == vertex_index_backend_t::art) art_ = std::make_unique<art_vertex_map_t>();
    try {
      grow_to_at_least(count);
    } catch (...) {
      // A failed constructor does not run this object's destructor.
      art_.reset();
      while (entries_) {
        auto* next = entries_->next;
        delete entries_;
        entries_ = next;
      }
      throw;
    }
  }
  ~vertex_table_t() {
    art_.reset();
    while (entries_) {
      auto* next = entries_->next;
      delete entries_;
      entries_ = next;
    }
  }
  vertex_table_t(const vertex_table_t&) = delete;
  vertex_table_t& operator=(const vertex_table_t&) = delete;
  vertex_index_backend_t backend() const { return backend_; }
  size_t size() const { return size_.load(std::memory_order_acquire); }
  size_t capacity() const { return art_ ? size() : chunks_.size() * chunk_size; }
  // Control/directory allocation estimate, excluding allocator and ART internal
  // nodes/epoch state. This is not a total-memory or RSS measurement.
  size_t directory_bytes() const {
    return art_ ? size() * (sizeof(entry_t) - sizeof(T)) :
        chunks_.capacity() * sizeof(std::unique_ptr<chunk_t>) +
        chunks_.size() * (sizeof(chunk_t) - chunk_size * sizeof(T));
  }
  T& operator[](size_t id) {
    if (id >= size()) throw std::out_of_range("Vertex index ID");
    if (art_) {
      auto* value = static_cast<T*>(art_->find(id));
      if (!value) throw std::logic_error("Missing ART vertex control");
      return *value;
    }
    return (*chunks_[id / chunk_size])[id % chunk_size];
  }
  const T& operator[](size_t id) const {
    return const_cast<vertex_table_t*>(this)->operator[](id);
  }
  template <class... Args> T& emplace_back(Args&&... args) {
    std::lock_guard lock(growth_mutex_);
    return append_unlocked(std::forward<Args>(args)...);
  }
  void grow_to_at_least(size_t count) {
    if (count > UINT32_MAX) throw std::overflow_error("Vertex ID exhausted");
    std::lock_guard lock(growth_mutex_);
    while (size() < count) append_unlocked();
  }
  void resize(size_t count) {
    if (count < size()) throw std::invalid_argument("Vertex index shrinking requires clear");
    grow_to_at_least(count);
  }
  void clear() {
    std::lock_guard lock(growth_mutex_);
    // Construct before discarding the old tree, for allocation-failure safety.
    auto fresh = art_ ? std::make_unique<art_vertex_map_t>() : nullptr;
    art_ = std::move(fresh);
    while (entries_) {
      auto* next = entries_->next;
      delete entries_;
      entries_ = next;
    }
    chunks_.clear();
    size_.store(0, std::memory_order_release);
  }
  auto begin() { return iterator_t<false>{this, 0}; }
  auto end() { return iterator_t<false>{this, size()}; }
  auto begin() const { return iterator_t<true>{this, 0}; }
  auto end() const { return iterator_t<true>{this, size()}; }
};
}
#endif
