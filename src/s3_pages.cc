#include "bw_graph/storage/s3_pages.h"
#include <nlohmann/json.hpp>
#include <stdexcept>
namespace bw_graph {
namespace {
std::string prefix(const std::filesystem::path& path) { return "pages/" + s3_path_key(path); }
}
s3_pages_t::s3_pages_t(const std::filesystem::path& directory, const std::array<uint32_t, 6>& sizes)
    : store_(std::make_shared<s3_store_t>()), prefix_(prefix(directory)),
      lock_(store_, prefix_ + "/owner"), sizes_(sizes) {
  std::vector<std::byte> raw;
  if (store_->get(prefix_ + "/manifest", raw)) {
    const auto doc = nlohmann::json::parse(reinterpret_cast<const char*>(raw.data()),
                                          reinterpret_cast<const char*>(raw.data() + raw.size()));
    if (doc.at("format") != "bw-s3-pages-1")
      throw std::runtime_error("Incompatible S3 page manifest");
    const auto& sizes = doc.at("sizes");
    const auto& counts = doc.at("counts");
    if (!sizes.is_array() || sizes.size() != sizes_.size() ||
        !counts.is_array() || counts.size() != counts_.size())
      throw std::runtime_error("Invalid S3 page manifest arrays");
    for (size_t i = 0; i < sizes_.size(); ++i) {
      // Validate before narrowing: JSON conversion to uint32_t can wrap a
      // corrupted 64-bit count or silently truncate a floating-point value.
      if (!sizes[i].is_number_unsigned() || sizes[i].get<uint64_t>() != sizes_[i] ||
          !counts[i].is_number_unsigned() ||
          counts[i].get<uint64_t>() > uint64_t(page_id_t::slot_mask) + 1)
        throw std::runtime_error("Invalid S3 page size or count");
      counts_[i] = static_cast<uint32_t>(counts[i].get<uint64_t>());
    }
    generation_ = doc.at("generation").get<std::string>();
    if (generation_.size() != 64 || generation_.find_first_not_of("0123456789abcdef") != std::string::npos)
      throw std::runtime_error("Invalid S3 generation");
  } else {
    generation_ = store_->unique_id(); publish();
  }
}
void s3_pages_t::publish() {
  const auto text = nlohmann::json{{"format", "bw-s3-pages-1"}, {"sizes", sizes_},
                                  {"counts", counts_}, {"generation", generation_}}.dump();
  store_->put(prefix_ + "/manifest", std::as_bytes(std::span(text.data(), text.size())));
}
std::string s3_pages_t::page_key(page_id_t id) const {
  return prefix_ + '/' + generation_ + '/' + std::to_string(static_cast<unsigned>(id.space())) + '/' + std::to_string(id.slot());
}
void s3_pages_t::allocate(page_id_t first, uint32_t count) {
  std::lock_guard guard(allocation_);
  const auto space = static_cast<size_t>(first.space());
  if (counts_[space] != first.slot()) throw std::logic_error("S3 allocation mismatch");
  std::vector<std::byte> zero(sizes_[space]);
  for (uint32_t n = 0; n < count; ++n)
    store_->put(page_key(page_id_t::make(first.space(), first.slot() + n)), zero);
  counts_[space] += count;
  try { publish(); } catch (...) { counts_[space] -= count; throw; }
}
void s3_pages_t::read(page_id_t id, std::span<std::byte> target) const { store_->get_into(page_key(id), target); }
void s3_pages_t::write(page_id_t id, std::span<const std::byte> source) { store_->put(page_key(id), source); }
void s3_pages_t::reset(const std::filesystem::path& directory) {
  auto store = std::make_shared<s3_store_t>();
  const auto root = prefix(directory);
  s3_lock_t lock(store, root + "/owner");
  std::vector<std::byte> manifest;
  if (store->get(root + "/manifest", manifest)) {
    store->put(root + "/history/" + store->unique_id(), manifest);
    store->erase(root + "/manifest");
  }
  lock.release();
}
}
