#ifndef BW_GRAPH_IO_MULTISCALE_CSR_PAGE_H
#define BW_GRAPH_IO_MULTISCALE_CSR_PAGE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bw_graph {

// Standalone experimental page image. No dependency on page_t, global configuration,
// buffer pools, compression, MVCC, or the legacy CSR disk format.
// Capacity includes all metadata. Vertex membership is fixed at construction;
// growing beyond reserved edge slots requires rebuilding a page externally.
// Mutation requires external synchronization. Moved-from pages may only be
// assigned to or destroyed. Memory is uint32_t-aligned, not direct-I/O-aligned.
class multiscale_csr_page_t {
public:
  static constexpr std::array<uint32_t, 4> page_sizes{4096, 32768, 262144, 2097152};
  static constexpr uint32_t header_bytes = 32;
  static constexpr uint32_t vertex_bytes = 16;
  static constexpr uint32_t format_version = 1;

  struct vertex_input_t {
    uint32_t vertex_id;
    std::span<const uint32_t> neighbors;
    uint32_t reserved_edges{0};
  };

  struct vertex_record_t {
    uint32_t vertex_id;
    uint32_t degree;
    uint32_t neighbor_byte_offset; // Relative to the neighbor region, not the page.
    uint32_t edge_capacity;        // Includes reserved slots.
  };

  // Empty page, or build in input order. Duplicate vertex IDs are rejected;
  // neighbor order, duplicate edges, and self-loops are preserved.
  explicit multiscale_csr_page_t(uint32_t capacity, uint32_t page_id = 0);
  static multiscale_csr_page_t build(uint32_t capacity, uint32_t page_id,
                                   std::span<const vertex_input_t> vertices);

  uint32_t capacity_bytes() const { return words_[2]; }
  uint32_t page_id() const { return words_[3]; }
  uint32_t vertex_count() const { return words_[4]; }
  uint32_t edge_count() const { return words_[5]; }
  uint32_t used_bytes() const { return words_[6]; } // Includes reserved edge slots.
  vertex_record_t vertex(uint32_t vertex_slot) const;

  // Read-only view, valid until mutation, assignment, or destruction of this page.
  std::span<const uint32_t> neighbors(uint32_t vertex_slot) const;
  bool insert_edge(uint32_t vertex_slot, uint32_t destination); // False if slots full.
  bool erase_edge(uint32_t vertex_slot, uint32_t destination);  // Removes first match.

  // Exact-capacity little-endian image: 8 header words, 4 words per vertex,
  // adjacency arrays including reserved slots, then zero padding. Deserialize
  // validates the entire layout before exposing any views. No crash atomicity.
  // Header words: magic, version, capacity, opaque page ID, vertex count,
  // live edge count, used bytes, reserved (zero). Record words follow the order
  // in vertex_record_t. The returned byte vector owns a copy of the page image.
  std::vector<std::byte> serialize() const;
  static multiscale_csr_page_t deserialize(std::span<const std::byte> image);

private:
  static constexpr uint32_t magic = 0x3150434d; // "MCP1" in little endian.
  size_t record_word(uint32_t vertex_slot) const;
  size_t neighbor_word(uint32_t vertex_slot) const;
  void validate() const;
  // Host-endian words give aligned, type-safe adjacency spans. Serialization
  // explicitly encodes endian order; no C++ struct padding is written to disk.
  std::vector<uint32_t> words_;
};

} // namespace bw_graph

#endif
