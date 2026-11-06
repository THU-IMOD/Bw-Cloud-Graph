#include "bw_graph/io/multiscale_csr_page.h"
#include "bw_graph/io/io_index.h"
#include "bw_graph/io/io_delta.h"
#include "bw_graph/storage/multiscale_disk_manager.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <set>
#include <unistd.h>

using namespace bw_graph;

namespace {
constexpr std::array<page_space_t, 6> all_spaces{
    page_space_t::CSR_4K, page_space_t::CSR_32K, page_space_t::CSR_256K,
    page_space_t::CSR_2M, page_space_t::INDEX, page_space_t::DELTA};

class MultiscaleDiskFormatsTest : public ::testing::TestWithParam<bool> {
protected:
  void SetUp() override {
    std::string name = (std::filesystem::temp_directory_path() /
                        "bw-multiscale-disk-XXXXXX").string();
    const char* created = ::mkdtemp(name.data());
    ASSERT_NE(created, nullptr);
    directory = created;
    options.bypass_cache = GetParam();
    options.io_workers = 3;
    options.queue_capacity = 2; // Force backpressure on batches larger than the queue.
  }
  void TearDown() override { std::filesystem::remove_all(directory); }
  std::filesystem::path directory;
  multiscale_disk_options_t options;
};

TEST_P(MultiscaleDiskFormatsTest, RealPageFormats) {
  multiscale_disk_options_t opts = options;
  opts.index_page_size = BW_GRAPH_INDEX_PAGE_SIZE;
  opts.delta_page_size = BW_DELTA_PAGE_SIZE;
  multiscale_disk_manager_t disk(directory, opts);
  std::vector<page_id_t> ids;
  const std::vector<uint32_t> neighbors{8, 9, 10};
  const std::array<multiscale_csr_page_t::vertex_input_t, 1> vertices{{{7, neighbors, 1}}};
  for (size_t i = 0; i < 4; ++i) {
    auto id = disk.allocate_page(all_spaces[i]);
    ids.push_back(id);
    auto csr = multiscale_csr_page_t::build(disk.page_size(id.space()), id.raw(), vertices);
    auto bytes = csr.serialize();
    disk.write_page(id, bytes);
  }
  auto index_id = disk.allocate_page(page_space_t::INDEX);
  index_page_t index;
  ASSERT_TRUE(index.insert_block(ids[2].raw()));
  disk.write_page(index_id, std::as_bytes(std::span(index.get_data(), opts.index_page_size)));
  auto delta_id = disk.allocate_page(page_space_t::DELTA);
  delta_page_t delta(delta_id.raw());
  ASSERT_TRUE(delta.insert_delta(delta_record_t(EDGE, INSERT, 7, 8)));
  disk.write_page(delta_id, std::as_bytes(std::span(delta.get_data(), opts.delta_page_size)));
  disk.shutdown();

  multiscale_disk_manager_t reopened(directory, opts);
  for (auto id : ids) {
    auto raw = reopened.read_page(id);
    auto csr = multiscale_csr_page_t::deserialize(raw.bytes());
    EXPECT_EQ(csr.page_id(), id.raw());
    EXPECT_EQ(csr.capacity_bytes(), reopened.page_size(id.space()));
    EXPECT_EQ(std::vector<uint32_t>(csr.neighbors(0).begin(), csr.neighbors(0).end()), neighbors);
  }
  auto index_bytes = reopened.read_page(index_id);
  index_page_t restored_index;
  std::memcpy(restored_index.get_data(), index_bytes.bytes().data(), opts.index_page_size);
  restored_index.is_parsed_ = false; // The constructor parsed its original empty buffer.
  restored_index.self_parse();
  ASSERT_EQ(restored_index.get_blocks().size(), 1u);
  EXPECT_EQ(restored_index.get_blocks()[0], ids[2].raw());
  auto delta_bytes = reopened.read_page(delta_id);
  delta_page_t restored_delta(delta_id.raw());
  std::memcpy(restored_delta.get_data(), delta_bytes.bytes().data(), opts.delta_page_size);
  restored_delta.self_parse();
  auto records = restored_delta.get_related_records(7);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].second, 8u);
}

INSTANTIATE_TEST_SUITE_P(CacheModes, MultiscaleDiskFormatsTest, ::testing::Bool());
} // namespace
