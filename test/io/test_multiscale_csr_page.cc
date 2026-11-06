#include "bw_graph/io/multiscale_csr_page.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <gtest/gtest.h>

using Page = bw_graph::multiscale_csr_page_t;

namespace {
std::vector<uint32_t> values(std::span<const uint32_t> view) {
  return {view.begin(), view.end()};
}

void put_word(std::vector<std::byte>& image, size_t word, uint32_t value) {
  for (unsigned b = 0; b < 4; ++b) {
    image.at(word * 4 + b) = static_cast<std::byte>((value >> (8 * b)) & 0xff);
  }
}

class TemporaryFile {
public:
  TemporaryFile() {
    char name[] = "/tmp/bw-multiscale-page-XXXXXX";
    const int fd = mkstemp(name);
    if (fd < 0) {
      throw std::runtime_error("Cannot create test file");
    }
    close(fd);
    path = name;
  }
  ~TemporaryFile() { unlink(path.c_str()); }
  std::string path;
};

class MultiscalePageTest : public ::testing::TestWithParam<uint32_t> {};

TEST_P(MultiscalePageTest, EmptyAndBounds) {
  Page page(GetParam(), 123);
  EXPECT_EQ(page.capacity_bytes(), GetParam());
  EXPECT_EQ(page.page_id(), 123u);
  EXPECT_EQ(page.vertex_count(), 0u);
  EXPECT_EQ(page.edge_count(), 0u);
  EXPECT_EQ(page.used_bytes(), Page::header_bytes);
  EXPECT_EQ(Page::deserialize(page.serialize()).vertex_count(), 0u);
  EXPECT_THROW(page.vertex(0), std::out_of_range);
  EXPECT_THROW(page.neighbors(0), std::out_of_range);
  EXPECT_THROW(page.insert_edge(0, 1), std::out_of_range);
  EXPECT_THROW(page.erase_edge(0, 1), std::out_of_range);
}

TEST_P(MultiscalePageTest, BuildUpdateAndReopen) {
  const std::vector<uint32_t> a{7, 3, 7, 0, UINT32_MAX};
  const std::vector<uint32_t> b{42};
  const std::array<Page::vertex_input_t, 3> input{{{42, a, 2}, {9, {}, 1}, {0, b, 0}}};
  auto page = Page::build(GetParam(), 0x12345678, input);
  EXPECT_EQ(page.vertex(0).vertex_id, 42u);
  EXPECT_EQ(page.vertex(1).neighbor_byte_offset, 28u);
  EXPECT_EQ(page.vertex(2).neighbor_byte_offset, 32u);
  EXPECT_EQ(values(page.neighbors(0)), a);
  EXPECT_TRUE(page.neighbors(1).empty());
  EXPECT_TRUE(page.insert_edge(0, 10));
  EXPECT_TRUE(page.insert_edge(0, 11));
  const auto full = page.serialize();
  EXPECT_FALSE(page.insert_edge(0, 12));
  EXPECT_EQ(page.serialize(), full);
  EXPECT_TRUE(page.erase_edge(0, 7));
  EXPECT_EQ(values(page.neighbors(0)), (std::vector<uint32_t>{3, 7, 0, UINT32_MAX, 10, 11}));
  EXPECT_FALSE(page.erase_edge(0, 1234));
  EXPECT_TRUE(page.insert_edge(0, 99));
  EXPECT_TRUE(page.insert_edge(1, 8));
  EXPECT_TRUE(page.erase_edge(1, 8));
  EXPECT_TRUE(page.neighbors(1).empty());
  EXPECT_EQ(values(page.neighbors(2)), b); // Adjacent records must be untouched.
  EXPECT_EQ(page.edge_count(), 8u);
  EXPECT_THROW(page.neighbors(3), std::out_of_range);

  const auto image = page.serialize();
  ASSERT_EQ(image.size(), GetParam());
  EXPECT_EQ(image[0], std::byte{'M'});
  EXPECT_EQ(image[1], std::byte{'C'});
  EXPECT_EQ(image[2], std::byte{'P'});
  EXPECT_EQ(image[3], std::byte{'1'});
  EXPECT_EQ(image[12], std::byte{0x78}); // Explicit little-endian page ID.
  EXPECT_TRUE(std::all_of(image.begin() + page.used_bytes(), image.end(),
                          [](std::byte b) { return b == std::byte{0}; }));
  TemporaryFile file;
  {
    std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out);
    out.write(reinterpret_cast<const char*>(image.data()), image.size());
    out.close();
    ASSERT_TRUE(out);
  }
  std::vector<std::byte> disk_image(GetParam());
  {
    std::ifstream in(file.path, std::ios::binary);
    ASSERT_TRUE(in);
    in.read(reinterpret_cast<char*>(disk_image.data()), disk_image.size());
    ASSERT_EQ(in.gcount(), static_cast<std::streamsize>(disk_image.size()));
    EXPECT_EQ(in.peek(), std::char_traits<char>::eof());
  }
  const auto reopened = Page::deserialize(disk_image);
  EXPECT_EQ(reopened.serialize(), image);
  EXPECT_EQ(reopened.page_id(), page.page_id());
  EXPECT_EQ(reopened.edge_count(), page.edge_count());
  for (uint32_t slot = 0; slot < page.vertex_count(); ++slot) {
    EXPECT_EQ(reopened.vertex(slot).vertex_id, page.vertex(slot).vertex_id);
    EXPECT_EQ(values(reopened.neighbors(slot)), values(page.neighbors(slot)));
  }
}

TEST_P(MultiscalePageTest, ExactCapacityAndOverflow) {
  const size_t edge_capacity = (GetParam() - Page::header_bytes - Page::vertex_bytes) / 4;
  std::vector<uint32_t> neighbors(edge_capacity);
  std::iota(neighbors.begin(), neighbors.end(), 0u);
  std::array<Page::vertex_input_t, 1> input{{{1, neighbors, 0}}};
  const auto page = Page::build(GetParam(), 0, input);
  EXPECT_EQ(page.used_bytes(), GetParam());
  EXPECT_EQ(values(Page::deserialize(page.serialize()).neighbors(0)), neighbors);
  input[0].reserved_edges = 1;
  EXPECT_THROW(Page::build(GetParam(), 0, input), std::length_error);
  input[0].reserved_edges = UINT32_MAX;
  EXPECT_THROW(Page::build(GetParam(), 0, input), std::length_error);
  neighbors.push_back(17);
  input[0] = {1, neighbors, 0};
  EXPECT_THROW(Page::build(GetParam(), 0, input), std::length_error);

  std::vector<Page::vertex_input_t> vertices((GetParam() - Page::header_bytes) /
                                          Page::vertex_bytes);
  for (uint32_t i = 0; i < vertices.size(); ++i) {
    vertices[i] = {i, {}, 0};
  }
  auto dense = Page::build(GetParam(), 2, vertices);
  EXPECT_EQ(dense.vertex_count(), vertices.size());
  EXPECT_EQ(Page::deserialize(dense.serialize()).vertex_count(), vertices.size());
  EXPECT_EQ(dense.vertex(static_cast<uint32_t>(vertices.size() - 1)).vertex_id,
            vertices.size() - 1);
  vertices.push_back({UINT32_MAX, {}, 0});
  EXPECT_THROW(Page::build(GetParam(), 0, vertices), std::length_error);
}

TEST_P(MultiscalePageTest, RejectMalformedImages) {
  const std::vector<uint32_t> a{1, 2};
  const std::array<Page::vertex_input_t, 2> input{{{3, a, 1}, {4, a, 0}}};
  const auto image = Page::build(GetParam(), 5, input).serialize();
  // Each mutation targets a different validation invariant.
  const std::vector<std::pair<size_t, uint32_t>> corruptions{
      {0, 0}, {1, 99}, {2, 123}, {4, UINT32_MAX}, {5, 99}, {6, 0},
      {6, GetParam() + 4}, {7, 1}, {9, 4}, {10, 1}, {11, UINT32_MAX},
      {12, 3}, {14, 0}};
  for (const auto& [word, value] : corruptions) {
    auto bad = image;
    put_word(bad, word, value);
    EXPECT_THROW(Page::deserialize(bad), std::invalid_argument) << "word " << word;
  }
  auto short_image = image;
  short_image.pop_back();
  EXPECT_THROW(Page::deserialize(short_image), std::invalid_argument);
  auto long_image = image;
  long_image.push_back(std::byte{0});
  EXPECT_THROW(Page::deserialize(long_image), std::invalid_argument);
}

INSTANTIATE_TEST_SUITE_P(FourSizes, MultiscalePageTest,
                        ::testing::ValuesIn(Page::page_sizes));

TEST(MultiscalePage, RejectUnsupportedSizesAndDuplicateVertices) {
  for (const uint32_t size : {0u, 4095u, 8192u, 16384u, 65536u, 524288u, UINT32_MAX}) {
    EXPECT_THROW((void)Page{size}, std::invalid_argument);
  }
  const std::array<Page::vertex_input_t, 2> vertices{{{10, {}, 0}, {10, {}, 0}}};
  EXPECT_THROW(Page::build(4096, 0, vertices), std::invalid_argument);
  EXPECT_THROW(Page::deserialize({}), std::invalid_argument);
}

TEST(MultiscalePage, LargeDegreeAndByteOffsetAre32Bit) {
  std::vector<uint32_t> neighbors(70000);
  std::iota(neighbors.begin(), neighbors.end(), 100u);
  const std::array<Page::vertex_input_t, 2> vertices{{{7, neighbors, 1}, {8, neighbors, 0}}};
  auto page = Page::build(2097152, 9, vertices);
  EXPECT_EQ(page.edge_count(), 140000u);
  EXPECT_EQ(page.vertex(1).neighbor_byte_offset, 280004u);
  EXPECT_EQ(page.vertex(1).degree, 70000u);
  EXPECT_EQ(values(page.neighbors(1)), neighbors);
  EXPECT_TRUE(page.insert_edge(0, 33));
  const auto restored = Page::deserialize(page.serialize());
  EXPECT_EQ(restored.edge_count(), 140001u);
  EXPECT_EQ(restored.neighbors(0).back(), 33u);
  EXPECT_EQ(values(restored.neighbors(1)), neighbors);
}

TEST(MultiscalePage, HighVertexSlotCopyAndMove) {
  const std::vector<uint32_t> neighbors{123};
  std::vector<Page::vertex_input_t> vertices(70001);
  for (uint32_t i = 0; i < vertices.size(); ++i) {
    vertices[i] = {i, neighbors, 1};
  }
  auto page = Page::build(2097152, 88, vertices);
  auto copy = page;
  ASSERT_TRUE(copy.insert_edge(70000, 456));
  EXPECT_EQ(values(page.neighbors(70000)), neighbors);
  auto moved = std::move(copy);
  auto reopened = Page::deserialize(moved.serialize());
  EXPECT_EQ(reopened.vertex(70000).vertex_id, 70000u);
  EXPECT_EQ(values(reopened.neighbors(70000)), (std::vector<uint32_t>{123, 456}));
}
} // namespace
