#include "bw_graph/db/multiscale_store.h"
#include "bw_graph/db/db.h"
#include "bw_graph/common/config.h"
#include <gtest/gtest.h>
#include <thread>
#include <set>
#include <unistd.h>
using namespace bw_graph;
namespace {
class ColdStart : public ::testing::TestWithParam<vertex_index_backend_t> {
protected:
  std::filesystem::path path;
  vertex_index_backend_t saved;
  void SetUp() override {
    saved = BW_VERTEX_INDEX_BACKEND; BW_VERTEX_INDEX_BACKEND = GetParam();
    char name[] = "/tmp/bw-cold-XXXXXX"; path = mkdtemp(name);
  }
  void TearDown() override {
    BW_VERTEX_INDEX_BACKEND = saved;
    std::error_code e; std::filesystem::remove_all(path, e);
  }
  multiscale_buf_options_t options() {
    multiscale_buf_options_t o; for (auto& g : o.groups) g = {2, 8}; return o;
  }
};
TEST_P(ColdStart, EmptyOrphansVotingSmoAndReopen) {
  {
    multiscale_store_t db(path / "store", true, true, "does-not-exist", options(), 2, 2, true);
    EXPECT_EQ(db.vertex_count(), 0u); db.tat().validate();
    EXPECT_FALSE(db.update(0, 0, INSERT));
    std::vector<v_id_t> missing{0};
    EXPECT_THROW(db.append_vertex(missing), std::invalid_argument);
    EXPECT_EQ(db.vertex_count(), 0u);
    for (unsigned i = 0; i < 800; ++i) EXPECT_EQ(db.append_vertex(), i);
    EXPECT_EQ(db.tat().stats().leaves, 7u);
    db.consolidate_all(); EXPECT_EQ(db.tat().stats().leaves, 7u);
    EXPECT_TRUE(db.clone(0).empty());
    std::vector<v_id_t> neighbors{260, 261, 600};
    auto v = db.append_vertex(neighbors);
    EXPECT_EQ(v, 800u);
    EXPECT_EQ(db.location(v).anchor, db.location(260).anchor);
    EXPECT_EQ(db.location(v).anchor, db.location(261).anchor);
    EXPECT_NE(db.location(v).anchor, db.location(600).anchor);
    auto actual = db.clone(v); std::sort(actual.begin(), actual.end());
    EXPECT_EQ(actual, neighbors);
    const auto before = db.vertex_count();
    std::vector<v_id_t> invalid{0, 99999};
    EXPECT_THROW(db.append_vertex(invalid), std::invalid_argument);
    EXPECT_EQ(db.vertex_count(), before);
    EXPECT_TRUE(db.update(799, 260, INSERT));
    EXPECT_EQ(db.location(799).anchor, db.location(260).anchor);
    for (v_id_t dst = 1; dst < 800; ++dst) ASSERT_TRUE(db.update(0, dst, INSERT));
    db.consolidate_all(); db.tat().validate();
    EXPECT_EQ(db.clone(0).size(), 799u);
    db.close();
  }
  multiscale_store_t reopened(path / "store", false, true, "absent", options(), 2);
  EXPECT_EQ(reopened.vertex_count(), 801u);
  EXPECT_EQ(reopened.clone(0).size(), 799u);
  EXPECT_EQ(reopened.clone(799), (std::vector<v_id_t>{260}));
  EXPECT_EQ(reopened.append_vertex(), 801u);
  reopened.tat().validate();
}
TEST_P(ColdStart, ConcurrentAllocationAndEdges) {
  multiscale_store_t db(path / "store", true, true, "absent", options(), 2, 2, true);
  std::array<std::vector<v_id_t>, 4> ids;
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) threads.emplace_back([&, t] {
    for (int i = 0; i < 80; ++i) ids[t].push_back(db.append_vertex());
  });
  for (auto& thread : threads) thread.join();
  std::set<v_id_t> unique;
  for (auto& group : ids) unique.insert(group.begin(), group.end());
  EXPECT_EQ(unique.size(), 320u); EXPECT_EQ(db.tat().stats().leaves, 3u);
  threads.clear();
  for (int t = 0; t < 4; ++t) threads.emplace_back([&, t] {
    for (auto v : ids[t]) if (v) EXPECT_TRUE(db.update(v, 0, INSERT));
  });
  for (auto& thread : threads) thread.join();
  db.consolidate_all(); db.tat().validate();
  for (v_id_t v = 1; v < 320; ++v) EXPECT_EQ(db.clone(v), (std::vector<v_id_t>{0}));
}
TEST_P(ColdStart, EmptyReopenSpecifiedIdAndNoWorkers) {
  {
    multiscale_store_t db(path / "store", true, true, "absent", options(), 0, 2, true);
    db.close();
  }
  multiscale_store_t db(path / "store", false, true, "absent", options(), 0);
  EXPECT_EQ(db.vertex_count(), 0u);
  EXPECT_TRUE(db.insert_vertex(10));
  EXPECT_EQ(db.vertex_count(), 11u);
  EXPECT_FALSE(db.update(10, 0, INSERT)); // Reserved IDs are not live endpoints.
  EXPECT_TRUE(db.insert_vertex(0));
  EXPECT_EQ(db.tat().stats().leaves, 1u);
  EXPECT_TRUE(db.update(10, 0, INSERT));
  EXPECT_EQ(db.clone(10), (std::vector<v_id_t>{0}));
  EXPECT_EQ(db.append_vertex(), 11u);
  EXPECT_EQ(db.tat().stats().leaves, 2u); // The edge-bearing reception page was sealed.
}
TEST_P(ColdStart, SelfLoopsAndAbsentDeletesSealReceptionPage) {
  multiscale_store_t db(path / "store", true, true, "absent", options(), 2, 1, true);
  EXPECT_EQ(db.append_vertex(), 0u);
  EXPECT_TRUE(db.update(0, 0, DELETE));
  db.consolidate_all();
  EXPECT_EQ(db.append_vertex(), 1u);
  EXPECT_TRUE(db.update(1, 1, INSERT));
  db.consolidate_all();
  EXPECT_EQ(db.append_vertex(), 2u);
  EXPECT_EQ(db.clone(1), (std::vector<v_id_t>{1}));
  db.tat().validate();
}
TEST_P(ColdStart, ReopenResumesPartiallyFilledOrphanPage) {
  {
    multiscale_store_t db(path / "store", true, true, "absent", options(), 0, 2, true);
    for (int i = 0; i < 100; ++i) db.append_vertex();
    db.close();
  }
  multiscale_store_t db(path / "store", false, true, "absent", options(), 0);
  EXPECT_EQ(db.append_vertex(), 100u);
  EXPECT_EQ(db.tat().stats().leaves, 1u);
  EXPECT_EQ(db.location(0).anchor, db.location(100).anchor);
}
TEST_P(ColdStart, GiantAndRetainedBase) {
  multiscale_store_t db(path / "store", true, true, "absent", options(), 2, 2, true);
  for (int i = 0; i < 4000; ++i) db.append_vertex();
  for (v_id_t dst = 1; dst < 3500; ++dst) ASSERT_TRUE(db.update(0, dst, INSERT));
  db.consolidate_all();
  auto retained = db.read_neighbor(0);
  ASSERT_EQ(retained.size(), 3499u);
  ASSERT_TRUE(db.update(0, 3500, INSERT));
  db.consolidate_all();
  EXPECT_EQ(db.clone(0).size(), 3500u);
  EXPECT_EQ(db.base_edges(), 3500u);
  size_t count = 0; for ([[maybe_unused]] auto dst : retained) ++count;
  EXPECT_EQ(count, 3499u);
  db.tat().validate();
}
TEST_P(ColdStart, PublicCreateAndOpenWithoutGraphFile) {
  const auto csr = CSR_PREFIX, meta = META_PREFIX;
  struct restore_t {
    std::string csr, meta;
    ~restore_t() { CSR_PREFIX = csr; META_PREFIX = meta; }
  } restore{csr, meta};
  CSR_PREFIX = (path / "csr").string(); META_PREFIX = (path / "meta").string();
  {
    auto db = bw_graph_db_t::create_empty("cold_api");
    EXPECT_EQ(db->get_vertex_count(), 0u);
    EXPECT_EQ(db->append_vertex(), 0u);
    EXPECT_EQ(db->append_vertex(), 1u);
    std::vector<v_id_t> neighbors{0, 1};
    EXPECT_EQ(db->insert_vertex_with_neighbors(neighbors), 2u);
    db->close();
  }
  auto db = bw_graph_db_t::open("cold_api");
  EXPECT_EQ(db->get_vertex_count(), 3u);
  EXPECT_EQ(db->read_neighbor_clone(2).size(), 2u);
}
#ifdef BW_GRAPH_ENABLE_TRANSACTION
TEST_P(ColdStart, SnapshotSurvivesFirstAttachment) {
  multiscale_store_t db(path / "store", true, true, "absent", options(), 2, 2, true);
  for (int i = 0; i < 300; ++i) db.append_vertex();
  auto old = db.begin_snapshot();
  EXPECT_TRUE(db.update(299, 0, INSERT));
  EXPECT_TRUE(db.clone_at(299, old).empty());
  db.consolidate_all();
  EXPECT_TRUE(db.clone_at(299, old).empty());
  EXPECT_EQ(db.clone(299), (std::vector<v_id_t>{0}));
  EXPECT_THROW(db.append_vertex(), std::logic_error);
  db.end_snapshot(old);
  db.consolidate_all();
  EXPECT_EQ(db.clone(299), (std::vector<v_id_t>{0}));
}
#endif
TEST(ColdTat, RewriteAcrossParentsAndPruneEmptyBranches) {
  multiscale_tat_t tat(2);
  auto page = [](uint32_t slot) { return page_id_t::make(page_space_t::CSR_4K, slot); };
  auto edges = [](v_id_t) { return std::vector<v_id_t>{}; };
  std::vector<multiscale_tat_t::leaf_t> leaves;
  for (uint32_t i = 0; i < 16; ++i) leaves.push_back({page(i), {i}});
  tat.build(std::move(leaves), edges);
  std::array old{page(0), page(15)};
  tat.rewrite(old, {{page(32), {0}}, {page(33), {15}}}, edges);
  tat.validate();
  std::array next{page(32), page(33)};
  tat.rewrite(next, {{page(34), {0, 15}}}, edges);
  tat.validate();
  EXPECT_EQ(tat.stats().leaves, 15u);
  EXPECT_EQ(tat.members(page(34)), (std::vector<v_id_t>{0, 15}));
}
INSTANTIATE_TEST_SUITE_P(Backends, ColdStart,
    ::testing::Values(vertex_index_backend_t::array, vertex_index_backend_t::art));
}
