#ifndef BW_GRAPH_STORAGE_PAGE_ID_H
#define BW_GRAPH_STORAGE_PAGE_ID_H

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace bw_graph {

enum class page_space_t : uint8_t {
  CSR_4K = 0, CSR_32K = 1, CSR_256K = 2, CSR_2M = 3, INDEX = 4, DELTA = 5
};

// Encoded IDs are NOT legacy file-local page_no_t values.
class page_id_t {
public:
  static constexpr uint32_t slot_bits = 29;
  static constexpr uint32_t slot_mask = (uint32_t{1} << slot_bits) - 1;
  static constexpr uint32_t invalid_value = std::numeric_limits<uint32_t>::max();

  constexpr page_id_t() = default;
  static page_id_t make(page_space_t space, uint32_t slot) {
    if (static_cast<uint32_t>(space) >= 6 || slot > slot_mask) {
      throw std::out_of_range("Invalid page space or slot");
    }
    return page_id_t((static_cast<uint32_t>(space) << slot_bits) | slot);
  }
  static page_id_t from_raw(uint32_t value) {
    if ((value >> slot_bits) >= 6) {
      throw std::invalid_argument("Reserved or invalid Page ID");
    }
    return page_id_t(value);
  }
  constexpr bool valid() const { return (value_ >> slot_bits) < 6; }
  constexpr uint32_t raw() const { return value_; }
  page_space_t space() const {
    if (!valid()) throw std::invalid_argument("Invalid Page ID");
    return static_cast<page_space_t>(value_ >> slot_bits);
  }
  uint32_t slot() const {
    if (!valid()) throw std::invalid_argument("Invalid Page ID");
    return value_ & slot_mask;
  }
  bool operator==(const page_id_t&) const = default;

private:
  explicit constexpr page_id_t(uint32_t value) : value_(value) {}
  uint32_t value_{invalid_value};
};

} // namespace bw_graph
#endif
