#pragma once
#include "bw_graph/storage/s3_store.h"
#include "bw_graph/storage/page_id.h"
#include <array>
#include <mutex>
namespace bw_graph {
class s3_pages_t {
public:
  s3_pages_t(const std::filesystem::path& directory, const std::array<uint32_t, 6>& sizes);
  uint64_t count(size_t space) const { return counts_.at(space); }
  void allocate(page_id_t first, uint32_t count);
  void read(page_id_t id, std::span<std::byte> target) const;
  void write(page_id_t id, std::span<const std::byte> source);
  void close() { lock_.release(); }
  static void reset(const std::filesystem::path& directory);
private:
  void publish();
  std::string page_key(page_id_t id) const;
  std::shared_ptr<s3_store_t> store_;
  std::string prefix_, generation_;
  s3_lock_t lock_;
  std::array<uint32_t, 6> sizes_, counts_{};
  std::mutex allocation_;
};
}
