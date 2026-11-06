#pragma once

#include "bw_graph/common/type.h"
#include "bw_graph/storage/page_id.h"
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <vector>

namespace bw_graph {
// Resident TAT directory. Leaves address physical CSR pages; internal IDs are
// logical (above the 32-bit page-ID domain). Checkpoint packs the directory into
// the same versioned Index stream as the vertex index. No extra storage fd.
class multiscale_tat_t {
public:
  using id_t = uint64_t;
  struct leaf_t { page_id_t page; std::vector<v_id_t> vertices; };
  using edges_t = std::function<std::vector<v_id_t>(v_id_t)>;
  struct stats_t { size_t leaves, internal, height; uint64_t replacements, splits; };
  explicit multiscale_tat_t(size_t fanout = 256);
  void build(std::vector<leaf_t> leaves, const edges_t& edges);
  void replace(page_id_t old, std::vector<leaf_t> leaves, const edges_t& edges);
  void append(leaf_t leaf, const edges_t& edges);
  void set_members(page_id_t page, std::vector<v_id_t> members);
  void rewrite(std::span<const page_id_t> old, std::vector<leaf_t> leaves,
               const edges_t& edges);
  std::vector<v_id_t> members(page_id_t page) const;
  std::vector<leaf_t> leaves() const;
  stats_t stats() const;
  uint64_t memory_bytes() const;
  void validate() const;
  void encode(std::vector<std::byte>& bytes) const;
  void decode(std::span<const std::byte> bytes);
private:
  struct node_t {
    id_t parent{0};
    std::vector<id_t> children;
    std::vector<v_id_t> vertices;
  };
  static constexpr id_t first_internal = id_t{1} << 32;
  bool internal(id_t id) const { return id >= first_internal; }
  id_t make_node(std::vector<id_t> children, id_t parent);
  void rebalance(id_t node, const edges_t& edges);
  std::vector<id_t> topology_order(const std::vector<id_t>& children,
                                  const edges_t& edges) const;
  void collect(id_t node, std::vector<v_id_t>& vertices) const;
  void validate_locked() const;
  mutable std::mutex mutex_;
  std::map<id_t, node_t> nodes_;
  id_t root_{0}, next_{first_internal};
  size_t fanout_;
  uint64_t replacements_{0}, splits_{0};
};
} // namespace bw_graph
