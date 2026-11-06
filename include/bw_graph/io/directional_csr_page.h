#pragma once
#include "bw_graph/common/direction.h"
#include "bw_graph/io/multiscale_csr_page.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
namespace bw_graph {
// Native directed format: 40-byte header and 32-byte dual-direction vertex slots.
// Offsets in slots are relative to the common edge region. Little-endian host is
// required by the native store. The standalone MCP1 prototype remains independent.
class directional_csr_page_t {
public:
  static constexpr uint32_t magic = 0x3450434d, version = 1;
  static constexpr size_t header_bytes = 40, vertex_bytes = 32;
  struct input_t {
    uint32_t vertex_id;
    std::span<const uint32_t> neighbors;
    uint32_t reserved_edges{0};
    std::span<const uint32_t> incoming{};
  };
  static uint32_t word(std::span<const std::byte> b, size_t offset) {
    if (offset + 4 > b.size()) throw std::invalid_argument("Short directional CSR");
    uint32_t n; std::memcpy(&n, b.data() + offset, 4); return n;
  }
  static std::vector<std::byte> build(size_t size, uint32_t id,
                                    std::span<const input_t> vertices, uint64_t ts = 0) {
    if (std::find(multiscale_csr_page_t::page_sizes.begin(),
                  multiscale_csr_page_t::page_sizes.end(), size) ==
        multiscale_csr_page_t::page_sizes.end()) throw std::invalid_argument("CSR page size");
    size_t used = header_bytes + vertices.size() * vertex_bytes, edges = 0;
    std::unordered_set<uint32_t> ids;
    for (const auto& v : vertices) {
      if (!ids.insert(v.vertex_id).second) throw std::invalid_argument("Duplicate CSR vertex");
      used += v.neighbors.size_bytes() + v.incoming.size_bytes() + size_t(v.reserved_edges) * 4;
      edges += v.neighbors.size() + v.incoming.size();
    }
    if (used > size) throw std::length_error("Directed CSR page overflow");
    std::vector<std::byte> bytes(size);
    auto put = [&](size_t offset, uint32_t n) { std::memcpy(bytes.data() + offset, &n, 4); };
    put(0, magic); put(4, version); put(8, size); put(12, id);
    put(16, vertices.size()); put(20, edges); put(24, used);
    std::memcpy(bytes.data() + 32, &ts, 8);
    size_t offset = 0, data = header_bytes + vertices.size() * vertex_bytes;
    for (size_t i = 0; i < vertices.size(); ++i) {
      const auto& v = vertices[i]; const size_t slot = header_bytes + i * vertex_bytes;
      put(slot, v.vertex_id);
      for (unsigned d = 0; d < 2; ++d) {
        auto neighbors = d ? v.incoming : v.neighbors;
        auto capacity = neighbors.size() + (d ? 0 : v.reserved_edges);
        put(slot + 8 + d * 12, neighbors.size()); put(slot + 12 + d * 12, offset);
        put(slot + 16 + d * 12, capacity);
        if (!neighbors.empty()) std::memcpy(bytes.data() + data + offset,
                                            neighbors.data(), neighbors.size_bytes());
        offset += capacity * 4;
      }
    }
    return bytes;
  }
  static void validate_header(std::span<const std::byte> b, uint32_t id) {
    if (b.size() < header_bytes || word(b, 0) != magic || word(b, 4) != version ||
        word(b, 8) != b.size() || word(b, 12) != id || word(b, 28) ||
        uint64_t(header_bytes) + uint64_t(word(b, 16)) * vertex_bytes > word(b, 24) ||
        word(b, 24) > b.size() || word(b, 24) % 4)
      throw std::invalid_argument("Invalid directional CSR header");
  }
  static std::span<const std::byte> neighbors(std::span<const std::byte> b,
                                            uint32_t slot, direction_t dir) {
    validate_header(b, word(b, 12));
    if (slot >= word(b, 16)) throw std::out_of_range("Directed CSR vertex slot");
    const auto p = header_bytes + slot * vertex_bytes + 8 + uint32_t(dir) * 12;
    const auto n = word(b, p), offset = word(b, p + 4), capacity = word(b, p + 8);
    const auto data = header_bytes + size_t(word(b, 16)) * vertex_bytes;
    if (n > capacity || offset % 4 || data + uint64_t(offset) + uint64_t(capacity) * 4 > word(b, 24))
      throw std::invalid_argument("Invalid directional CSR slot");
    return b.subspan(data + offset, size_t(n) * 4);
  }
};
}
