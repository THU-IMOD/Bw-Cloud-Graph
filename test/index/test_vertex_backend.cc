#include "bw_graph/common/config.h"
#include "bw_graph/index/vertex_index.h"
#include "bw_graph/db/multiscale_store.h"
#include <gtest/gtest.h>
#include <fstream>
#include <thread>
#include <unistd.h>
using namespace bw_graph;
namespace {
struct backend_scope_t {
  vertex_index_backend_t saved = BW_VERTEX_INDEX_BACKEND;
  explicit backend_scope_t(vertex_index_backend_t b) { BW_VERTEX_INDEX_BACKEND = b; }
  ~backend_scope_t() { BW_VERTEX_INDEX_BACKEND = saved; }
};
struct temp_t {
  std::filesystem::path path;
  temp_t() { char p[] = "/tmp/bw-art-index-XXXXXX"; path = mkdtemp(p); }
  ~temp_t() { std::error_code e; std::filesystem::remove_all(path, e); }
};
class VertexBackend : public ::testing::TestWithParam<vertex_index_backend_t> {};
TEST_P(VertexBackend, StableControlsConcurrentGrowthAndUpdates) {
  backend_scope_t backend(GetParam());
  vertex_table_t<std::atomic<uint64_t>> table(1);
  auto* first = &table[0];
  table[0] = 123;
  std::atomic<bool> done{false}, failed{false};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) readers.emplace_back([&] {
    while (!done.load()) {
      if (&table[0] != first || table[0].load() != 123) failed = true;
      const auto n = table.size();
      if (n > 1) table[n - 1].fetch_add(1);
    }
  });
  for (size_t i = 1; i < 4097; ++i) table.grow_to_at_least(i + 1);
  done = true;
  for (auto& reader : readers) reader.join();
  EXPECT_FALSE(failed);
  EXPECT_EQ(&table[0], first);
  EXPECT_EQ(table.backend(), GetParam());
  EXPECT_THROW(table[4097], std::out_of_range);
  EXPECT_THROW(table.grow_to_at_least(uint64_t(UINT32_MAX) + 1), std::overflow_error);
  table.clear();
  EXPECT_EQ(table.size(), 0u);
  table.grow_to_at_least(2);
  EXPECT_EQ(table[0].load(), 0u);
}
TEST_P(VertexBackend, VersionsCASAndCrossBackendPersistence) {
  backend_scope_t backend(GetParam());
  temp_t temp;
  vertex_index_t index(64);
  EXPECT_EQ(index.items.backend(), GetParam());
  index.update_vertex_loc(0, 7, 12);
  auto old = index.protect_vertex_version(0);
  auto stale = index.prepare_vertex_version(0, 8, 16);
  index.update_vertex_loc(0, 9, 20);
  EXPECT_FALSE(index.publish_prepared_version(0, stale));
  EXPECT_EQ(old.page_no(), 7u);
  EXPECT_EQ(index.get_vertex_location(0), vertex_loc_t(9, 20));
  auto* control = &index.items[0];
  for (size_t i = 0; i < 1024; ++i) index.allocate_new_vertex();
  EXPECT_EQ(&index.items[0], control);
  index.delta_heads_[0] = {42, 3};
  index.items[1].mark_deleted();
  ASSERT_TRUE(index.save_to_file((temp.path / "index").string(), false));
  BW_VERTEX_INDEX_BACKEND = GetParam() == vertex_index_backend_t::art ?
      vertex_index_backend_t::array : vertex_index_backend_t::art;
  vertex_index_t reopened;
  ASSERT_TRUE(reopened.load_from_file((temp.path / "index").string()));
  EXPECT_EQ(reopened.get_vertex_location(0), vertex_loc_t(9, 20));
  EXPECT_EQ(reopened.delta_heads_[0].page_no, 42u);
  EXPECT_TRUE(reopened.items[1].deleted());
  EXPECT_EQ(reopened.items.size(), 1088u);
  old.reset();
  index.wait_for_version_gc();
}
TEST_P(VertexBackend, NativeCrossBackendReopenAndGenerationLifetime) {
  backend_scope_t backend(GetParam());
  temp_t temp;
  auto graph = temp.path / "graph";
  { std::ofstream out(graph); out << "t 3000 2000\n";
    for (unsigned v = 1; v <= 2000; ++v) out << "e 0 " << v << '\n'; }
  multiscale_buf_options_t options;
  for (auto& g : options.groups) g = {2, 8};
  {
    multiscale_store_t db(temp.path / "db", true, true, graph.string(), options, 1, 1);
    EXPECT_EQ(db.vertex_index_backend(), GetParam());
    auto old = db.read_neighbor(0);
    ASSERT_TRUE(db.update(0, 1, DELETE));
    ASSERT_TRUE(db.update(0, 2001, INSERT));
    db.consolidate_all();
    EXPECT_EQ(*old.begin(), 1u);
    auto values = db.clone(0);
    EXPECT_EQ(values.size(), 2000u);
    EXPECT_EQ(std::count(values.begin(), values.end(), 1), 0);
    EXPECT_EQ(std::count(values.begin(), values.end(), 2001), 1);
    EXPECT_EQ(db.append_vertex(), 3000u);
  }
  BW_VERTEX_INDEX_BACKEND = GetParam() == vertex_index_backend_t::art ?
      vertex_index_backend_t::array : vertex_index_backend_t::art;
  multiscale_store_t db(temp.path / "db", false, true, graph.string(), options);
  EXPECT_EQ(db.vertex_count(), 3001u);
  auto values = db.clone(0);
  EXPECT_EQ(std::count(values.begin(), values.end(), 1), 0);
  EXPECT_EQ(std::count(values.begin(), values.end(), 2001), 1);
}
INSTANTIATE_TEST_SUITE_P(ArrayAndArt, VertexBackend,
    ::testing::Values(vertex_index_backend_t::array, vertex_index_backend_t::art));
TEST(VertexBackendConfig, SelectionFallbackAndInvalidValue) {
  backend_scope_t scope(vertex_index_backend_t::array);
  temp_t temp;
  auto file = temp.path / "config.yaml";
  { std::ofstream out(file); out << "index: {vertex_index: art}\n"; }
  load_yaml_config(file.string());
  EXPECT_EQ(BW_VERTEX_INDEX_BACKEND, vertex_index_backend_t::art);
  vertex_table_t<uint64_t> existing(1);
  { std::ofstream out(file); out << "{}\n"; }
  load_yaml_config(file.string());
  EXPECT_EQ(BW_VERTEX_INDEX_BACKEND, vertex_index_backend_t::array);
  EXPECT_EQ(existing.backend(), vertex_index_backend_t::art);
  for (const auto* yaml : {"index: {vertex_index: typo}", "index: []"}) {
    { std::ofstream out(file); out << yaml; }
    EXPECT_THROW(load_yaml_config(file.string()), std::invalid_argument);
    EXPECT_EQ(BW_VERTEX_INDEX_BACKEND, vertex_index_backend_t::array);
  }
}
}
