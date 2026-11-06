#include "bw_graph/io/multiscale_csr_page.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace bw_graph {
namespace {
bool supported(size_t size) {
  const auto& sizes = multiscale_csr_page_t::page_sizes;
  return std::find(sizes.begin(), sizes.end(), size) != sizes.end();
}
} // namespace

multiscale_csr_page_t::multiscale_csr_page_t(uint32_t capacity, uint32_t page_id) {
  if (!supported(capacity)) {
    throw std::invalid_argument("Unsupported multiscale CSR page size");
  }
  words_.resize(capacity / sizeof(uint32_t), 0);
  words_[0] = magic;
  words_[1] = format_version;
  words_[2] = capacity;
  words_[3] = page_id;
  words_[6] = header_bytes;
}

multiscale_csr_page_t multiscale_csr_page_t::build(
    uint32_t capacity, uint32_t page_id, std::span<const vertex_input_t> vertices) {
  multiscale_csr_page_t page(capacity, page_id);
  if (vertices.size() > (capacity - header_bytes) / vertex_bytes) {
    throw std::length_error("Vertex records exceed page capacity");
  }
  uint64_t used = header_bytes + vertices.size() * vertex_bytes;
  uint32_t edges = 0;
  std::unordered_set<uint32_t> ids;
  ids.reserve(vertices.size());
  for (const auto& v : vertices) {
    if (!ids.insert(v.vertex_id).second) {
      throw std::invalid_argument("Duplicate vertex ID");
    }
    if (v.neighbors.size() > std::numeric_limits<uint32_t>::max()) {
      throw std::length_error("Degree exceeds 32-bit range");
    }
    const uint64_t slots = uint64_t(v.neighbors.size()) + v.reserved_edges;
    used += slots * sizeof(uint32_t);
    if (used > capacity) {
      throw std::length_error("Adjacency and reservation exceed page capacity");
    }
    edges += static_cast<uint32_t>(v.neighbors.size()); // Bounded by page capacity.
  }
  page.words_[4] = static_cast<uint32_t>(vertices.size());
  page.words_[5] = edges;
  page.words_[6] = static_cast<uint32_t>(used);
  uint32_t offset = 0;
  const size_t neighbor_start = header_bytes / 4 + vertices.size() * 4;
  for (uint32_t i = 0; i < vertices.size(); ++i) {
    const auto& v = vertices[i];
    const size_t r = page.record_word(i);
    const uint32_t degree = static_cast<uint32_t>(v.neighbors.size());
    page.words_[r] = v.vertex_id;
    page.words_[r + 1] = degree;
    page.words_[r + 2] = offset;
    page.words_[r + 3] = degree + v.reserved_edges;
    std::copy(v.neighbors.begin(), v.neighbors.end(),
              page.words_.begin() + neighbor_start + offset / 4);
    offset += (degree + v.reserved_edges) * sizeof(uint32_t);
  }
  return page;
}

size_t multiscale_csr_page_t::record_word(uint32_t slot) const {
  if (slot >= vertex_count()) {
    throw std::out_of_range("Vertex slot outside multiscale CSR page");
  }
  return header_bytes / 4 + size_t(slot) * 4;
}

multiscale_csr_page_t::vertex_record_t multiscale_csr_page_t::vertex(uint32_t slot) const {
  const size_t r = record_word(slot);
  return {words_[r], words_[r + 1], words_[r + 2], words_[r + 3]};
}

size_t multiscale_csr_page_t::neighbor_word(uint32_t slot) const {
  return header_bytes / 4 + size_t(vertex_count()) * 4 + vertex(slot).neighbor_byte_offset / 4;
}

std::span<const uint32_t> multiscale_csr_page_t::neighbors(uint32_t slot) const {
  return {words_.data() + neighbor_word(slot), vertex(slot).degree};
}

bool multiscale_csr_page_t::insert_edge(uint32_t slot, uint32_t destination) {
  const auto v = vertex(slot);
  if (v.degree == v.edge_capacity) {
    return false;
  }
  words_[neighbor_word(slot) + v.degree] = destination;
  ++words_[record_word(slot) + 1];
  ++words_[5];
  return true;
}

bool multiscale_csr_page_t::erase_edge(uint32_t slot, uint32_t destination) {
  const auto ns = neighbors(slot);
  const auto it = std::find(ns.begin(), ns.end(), destination);
  if (it == ns.end()) {
    return false;
  }
  const size_t first = neighbor_word(slot);
  const size_t index = static_cast<size_t>(it - ns.begin());
  for (size_t i = index + 1; i < ns.size(); ++i) {
    words_[first + i - 1] = words_[first + i];
  }
  words_[first + ns.size() - 1] = 0;
  --words_[record_word(slot) + 1];
  --words_[5];
  return true;
}

std::vector<std::byte> multiscale_csr_page_t::serialize() const {
  std::vector<std::byte> image(capacity_bytes());
  for (size_t i = 0; i < words_.size(); ++i) {
    for (unsigned b = 0; b < 4; ++b) {
      image[i * 4 + b] = static_cast<std::byte>((words_[i] >> (8 * b)) & 0xff);
    }
  }
  return image;
}

multiscale_csr_page_t multiscale_csr_page_t::deserialize(std::span<const std::byte> image) {
  if (!supported(image.size())) {
    throw std::invalid_argument("Incorrect multiscale CSR image length");
  }
  multiscale_csr_page_t page(static_cast<uint32_t>(image.size()));
  for (size_t i = 0; i < page.words_.size(); ++i) {
    uint32_t word = 0;
    for (unsigned b = 0; b < 4; ++b) {
      word |= std::to_integer<uint32_t>(image[i * 4 + b]) << (8 * b);
    }
    page.words_[i] = word;
  }
  page.validate();
  return page;
}

void multiscale_csr_page_t::validate() const {
  if (words_[0] != magic || words_[1] != format_version || words_[7] != 0 ||
      capacity_bytes() != words_.size() * sizeof(uint32_t)) {
    throw std::invalid_argument("Invalid multiscale CSR header or unsupported format");
  }
  const uint64_t neighbor_start = header_bytes + uint64_t(vertex_count()) * vertex_bytes;
  if (neighbor_start > used_bytes() || used_bytes() > capacity_bytes()) {
    throw std::invalid_argument("Invalid multiscale CSR record bounds");
  }
  uint64_t offset = 0;
  uint64_t edges = 0;
  std::unordered_set<uint32_t> ids;
  ids.reserve(vertex_count());
  for (uint32_t i = 0; i < vertex_count(); ++i) {
    const auto v = vertex(i);
    if (!ids.insert(v.vertex_id).second || v.degree > v.edge_capacity ||
        v.neighbor_byte_offset != offset) {
      throw std::invalid_argument("Invalid multiscale CSR vertex record");
    }
    offset += uint64_t(v.edge_capacity) * sizeof(uint32_t);
    edges += v.degree;
    if (neighbor_start + offset > used_bytes()) {
      throw std::invalid_argument("Invalid multiscale CSR adjacency bounds");
    }
  }
  if (neighbor_start + offset != used_bytes() || edges != edge_count()) {
    throw std::invalid_argument("Inconsistent multiscale CSR counts");
  }
}

} // namespace bw_graph
