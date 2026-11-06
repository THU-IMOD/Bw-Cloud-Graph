#include "bw_graph/db/db.h"
#include <gtest/gtest.h>
#include <fstream>
#include <unistd.h>
#include <set>

TEST(LegacySmoAnchors, IdBuildKeepsGiantSlotsAndFullConsolidationPreservesOtherVertices) {
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  GTEST_SKIP() << "Non-transaction legacy regression; transaction visibility has separate suites";
#else
  using namespace bw_graph;
  char pattern[] = "/tmp/bw-legacy-anchor-XXXXXX";
  const char* created = mkdtemp(pattern);
  ASSERT_NE(created, nullptr);
  const std::filesystem::path root(created), previous = std::filesystem::current_path();
  struct cleanup_t {
    std::filesystem::path previous, root;
    ~cleanup_t() { std::filesystem::current_path(previous); std::filesystem::remove_all(root); }
  } cleanup{previous, root};
  std::filesystem::create_directory(root / "data");
  { std::ofstream out(root / "data/anchors.graph"); out << "t 5000 602\ne 0 1\ne 1 2\n";
    for (auto src : {100, 1000, 4000})
      for (int dst = 2000; dst < 2200; ++dst) out << "e " << src << ' ' << dst << '\n'; }
  std::filesystem::current_path(root);
  BW_MULTISCALE_ENGINE = false;
  CSR_PREFIX = DELTA_PREFIX = META_PREFIX = (root / "db").string();
  BW_GRAPH_PAGE_SIZE = 4096; BW_GRAPH_GIANT_DEGREE_BOUND = 64;
  BW_BUFFER_CHUNK_COUNT = BW_DELTA_BUFFER_CHUNK_COUNT = 2;
  BW_BUFFER_CHUNK_SIZE = BW_DELTA_BUFFER_CHUNK_SIZE = 128;
  BW_GRAPH_CONSOLIDATION_WORKER_COUNT = 1;
  BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = 100000; // Force must handle below-threshold records.
  BW_CSR_FLUSH_INTERVAL_MS = BW_DELTA_FLUSH_INTERVAL_MS = 0;
  {
    bw_graph_db_t db("anchors", true, true, false, true);
    for (v_id_t v : {100, 1000, 4000}) {
      const auto loc = db.vertex_index->get_vertex_location(v);
      auto* page = db.buffer_pool->buf_page_read(loc.first, db.disk_manager);
      auto slots = page->get_vertices();
      EXPECT_LT(loc.second, slots.size());
      if (loc.second < slots.size()) {
        EXPECT_EQ(slots[loc.second].vertex_id, v);
      }
      page->r_unlatch();
    }
    ASSERT_TRUE(db.insert_edge(1, 4999)); ASSERT_TRUE(db.insert_edge(100, 4900));
    db.consolidate_all_pages();
    ASSERT_TRUE(db.insert_edge(4000, 4901)); db.consolidate_all_pages();
    auto neighbors = db.read_neighbor_clone(1);
    EXPECT_NE(std::find(neighbors.begin(), neighbors.end(), 4999), neighbors.end());
    EXPECT_FALSE(db.vertex_index->delta_heads_[1].is_valid());
    auto [base, page] = db.read_neighbor_legacy(1);
    EXPECT_NE(std::find(base.begin(), base.end(), 4999), base.end());
    page->r_unlatch();
    EXPECT_EQ(db.read_neighbor_clone(100).size(), 201u);
    EXPECT_EQ(db.read_neighbor_clone(4000).size(), 201u);
  }
  bw_graph_db_t reopened("anchors", false, false, false, true);
  EXPECT_EQ(reopened.read_neighbor_clone(100).size(), 201u);
  EXPECT_EQ(reopened.read_neighbor_clone(1).size(), 3u);
#endif
}

TEST(LegacySmoAnchors, PartitionTatSurvivesReopenAndRepeatedSmo) {
#ifdef BW_GRAPH_ENABLE_TRANSACTION
  GTEST_SKIP() << "Non-transaction reference layout regression";
#else
  using namespace bw_graph;
  char pattern[] = "/tmp/bw-legacy-tat-XXXXXX";
  auto* created = mkdtemp(pattern);
  ASSERT_NE(created, nullptr);
  const std::filesystem::path root(created), previous = std::filesystem::current_path();
  struct cleanup_t {
    std::filesystem::path previous, root;
    ~cleanup_t() { std::filesystem::current_path(previous); std::filesystem::remove_all(root); }
  } cleanup{previous, root};
  std::filesystem::create_directory(root / "data");
  { std::ofstream out(root / "data/tat.graph"); out << "t 160 159\n";
    for (int v = 1; v < 160; ++v) out << "e " << v - 1 << ' ' << v << '\n'; }
  std::filesystem::current_path(root);
  BW_MULTISCALE_ENGINE = false;
  CSR_PREFIX = DELTA_PREFIX = META_PREFIX = (root / "db").string();
  BW_GRAPH_PAGE_SIZE = 4096; BW_GRAPH_INDEX_PAGE_SIZE = 4096;
  BW_GRAPH_INDEX_MAX_BLOCKS = 4; BW_GRAPH_GIANT_DEGREE_BOUND = 1024;
  BW_GRAPH_COARSENING_WEIGHT = 24; BW_GRAPH_REDO_PARTITION = true;
  BW_GRAPH_USING_NATIVE_PARTITION = false; BW_GRAPH_LEVELED = true;
  BW_BUFFER_CHUNK_COUNT = BW_DELTA_BUFFER_CHUNK_COUNT = 2;
  BW_BUFFER_CHUNK_SIZE = BW_DELTA_BUFFER_CHUNK_SIZE = 128;
  BW_GRAPH_CONSOLIDATION_WORKER_COUNT = 1; BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = 100000;
  BW_CSR_FLUSH_INTERVAL_MS = BW_DELTA_FLUSH_INTERVAL_MS = 0;
  auto check = [](bw_graph_db_t& db) {
    std::set<page_no_t> seen, leaves;
    std::function<void(index_page_t*, page_no_t)> visit = [&](auto* page, auto parent) {
      ASSERT_TRUE(seen.insert(page->get_page_no()).second);
      ASSERT_EQ(page->parent_page_no_, parent);
      for (auto c : page->get_blocks()) {
        auto it = db.index_page_map.find(c);
        if (it != db.index_page_map.end()) visit(it->second, page->get_page_no());
        else ASSERT_TRUE(leaves.insert(c).second);
      }
    };
    ASSERT_NE(db.root_index_page, nullptr);
    visit(db.root_index_page, 0);
    ASSERT_EQ(seen.size(), db.index_page_map.size());
    for (v_id_t v = 0; v < db.get_vertex_count(); ++v)
      ASSERT_TRUE(leaves.contains(db.vertex_index->get_vertex_location(v).first));
  };
  {
    bw_graph_db_t db("tat", true, true, false, false, true, true);
    check(db);
    for (v_id_t src = 0; src < 40; ++src)
      for (v_id_t dst = 80; dst < 150; ++dst) ASSERT_TRUE(db.insert_edge(src, dst));
    db.consolidate_all_pages(); check(db);
    ASSERT_TRUE(db.insert_edge(0, 159)); db.consolidate_all_pages(); check(db);
  }
  {
    bw_graph_db_t db("tat", false, false, false, false, true, true);
    check(db);
    auto values = db.read_neighbor_clone(0);
    EXPECT_EQ(values.size(), 72u);
    ASSERT_TRUE(db.insert_edge(0, 158)); db.consolidate_all_pages(); check(db);
    EXPECT_EQ(db.read_neighbor_clone(0).size(), 73u);
  }
  // Rebuild a missing directory from persisted vertex locations and borrowed frames.
  ASSERT_TRUE(std::filesystem::remove(index_page_path("tat")));
  {
    bw_graph_db_t db("tat", false, false, false, false, true, true);
    check(db);
    EXPECT_EQ(db.read_neighbor_clone(0).size(), 73u);
  }
#endif
}
