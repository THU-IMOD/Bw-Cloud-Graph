#ifndef BW_GRAPH_IO_MULTISCALE_CSR_PAGE_VIEW_H
#define BW_GRAPH_IO_MULTISCALE_CSR_PAGE_VIEW_H

#include "bw_graph/io/multiscale_csr_page.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace bw_graph {
// Borrowed, read-only view of serialized bytes. Keep the read guard alive throughout use.
// Construction checks only the header; each vertex access checks that record's bounds.
// This does not certify global uniqueness, record overlap, or summed degrees: use the
// owning deserialize() for full validation of untrusted images. No allocation or copy.
class multiscale_csr_page_view_t {
public:
  class neighbors_t {
  public:
    size_t size() const { return bytes_.size() / 4; }
    bool empty() const { return bytes_.empty(); }
    uint32_t operator[](size_t i) const {
      if (i >= size()) throw std::out_of_range("Neighbor index");
      return word(bytes_.data() + i * 4);
    }
  private:
    friend class multiscale_csr_page_view_t;
    explicit neighbors_t(std::span<const std::byte> bytes) : bytes_(bytes) {}
    std::span<const std::byte> bytes_;
  };
  explicit multiscale_csr_page_view_t(std::span<const std::byte> bytes) : bytes_(bytes) {
    if (bytes.size() < 32) throw std::invalid_argument("Short CSR header");
    const auto& sizes = multiscale_csr_page_t::page_sizes;
    if (get(0) != 0x3150434d || get(1) != multiscale_csr_page_t::format_version ||
        get(2) != bytes.size() || std::find(sizes.begin(), sizes.end(), get(2)) == sizes.end() ||
        get(7) != 0 || used_bytes() > bytes.size() || used_bytes() % 4 ||
        uint64_t{32} + uint64_t{vertex_count()} * 16 > used_bytes() ||
        uint64_t{edge_count()} * 4 > used_bytes() - (uint64_t{32} + uint64_t{vertex_count()} * 16))
      throw std::invalid_argument("Invalid CSR header");
  }
  uint32_t page_id() const { return get(3); }
  uint32_t vertex_count() const { return get(4); }
  uint32_t edge_count() const { return get(5); }
  uint32_t used_bytes() const { return get(6); }
  multiscale_csr_page_t::vertex_record_t vertex(uint32_t slot) const {
    if (slot >= vertex_count()) throw std::out_of_range("Vertex slot");
    const size_t offset = 8 + size_t{slot} * 4;
    multiscale_csr_page_t::vertex_record_t record{
        get(offset), get(offset + 1), get(offset + 2), get(offset + 3)};
    if (record.degree > record.edge_capacity || record.neighbor_byte_offset % 4 ||
        uint64_t{32} + uint64_t{vertex_count()} * 16 + record.neighbor_byte_offset +
            uint64_t{record.edge_capacity} * 4 > used_bytes())
      throw std::invalid_argument("Invalid CSR vertex record");
    return record;
  }
  neighbors_t neighbors(uint32_t slot) const {
    const auto record = vertex(slot);
    return neighbors_t(bytes_.subspan(32 + size_t{vertex_count()} * 16 +
        record.neighbor_byte_offset, size_t{record.degree} * 4));
  }
private:
  static uint32_t word(const std::byte* p) {
    uint32_t result;
    std::memcpy(&result, p, sizeof(result)); // Type/lifetime-safe, compiles to a load.
    if constexpr (std::endian::native == std::endian::big)
      result = ((result & 0xff) << 24) | ((result & 0xff00) << 8) |
               ((result >> 8) & 0xff00) | (result >> 24);
    return result;
  }
  uint32_t get(size_t offset) const { return word(bytes_.data() + offset * 4); }
  std::span<const std::byte> bytes_;
};
} // namespace bw_graph
#endif
