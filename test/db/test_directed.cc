#include "bw_graph/db/multiscale_store.h"
#include "bw_graph/io/directional_csr_page.h"
#include "bw_graph/common/config.h"
#include <gtest/gtest.h>
#include <fstream>
#include <thread>
#include <unistd.h>
using namespace bw_graph;
namespace {
std::vector<v_id_t> sorted(std::vector<v_id_t> v) { std::sort(v.begin(), v.end()); return v; }
struct Directed : ::testing::TestWithParam<vertex_index_backend_t> {
  std::filesystem::path path;
  vertex_index_backend_t saved;
  std::optional<graph_layout_t> saved_layout;
  void SetUp() override {
    char name[] = "/tmp/bw-directed-XXXXXX"; path = mkdtemp(name);
    saved_layout = BW_GRAPH_LAYOUT;
    saved = BW_VERTEX_INDEX_BACKEND; BW_VERTEX_INDEX_BACKEND = GetParam();
  }
  void TearDown() override {
    BW_GRAPH_LAYOUT = saved_layout;
    BW_VERTEX_INDEX_BACKEND = saved;
    std::filesystem::remove_all(path);
  }
  multiscale_buf_options_t options() {
    multiscale_buf_options_t o; for (auto& g : o.groups) g = {1, 8}; return o;
  }
};
TEST(DirectedFormat, BothDirectionsEveryScaleAndMalformedOffsets) {
  for (auto size : multiscale_csr_page_t::page_sizes) {
    std::vector<v_id_t> out{2, 3}, in{4, 7, 9};
    directional_csr_page_t::input_t input{1, out, 2, in};
    auto bytes = directional_csr_page_t::build(size, 123, {&input, 1}, 456);
    for (auto d : {direction_t::out, direction_t::in}) {
      auto span = directional_csr_page_t::neighbors(bytes, 0, d);
      std::vector<v_id_t> actual(span.size() / 4);
      std::memcpy(actual.data(), span.data(), span.size());
      EXPECT_EQ(actual, d == direction_t::out ? out : in);
    }
    uint32_t invalid = UINT32_MAX; std::memcpy(bytes.data() + 64, &invalid, 4);
    EXPECT_THROW(directional_csr_page_t::neighbors(bytes, 0, direction_t::in), std::invalid_argument);
  }
}
TEST_P(Directed, FileDeltaSmoAndReopen) {
  auto file = path / "input.graph";
  { std::ofstream out(file); out << "t 6 5\ne 0 1\ne 1 2\ne 3 1\ne 2 2\ne 0 1\n"; }
  {
    multiscale_store_t db(path / "store", true, true, file.string(), options(), 2, 1);
    EXPECT_EQ(db.base_edges(), 4u);
    EXPECT_TRUE(db.clone(0, direction_t::in).empty());
    EXPECT_EQ(db.clone(1), (std::vector<v_id_t>{2}));
    EXPECT_EQ(sorted(db.clone(1, direction_t::in)), (std::vector<v_id_t>{0, 3}));
    auto old = db.read_neighbor(1, direction_t::in);
    ASSERT_TRUE(db.update(0, 1, DELETE)); ASSERT_TRUE(db.update(4, 1, INSERT));
    EXPECT_EQ(sorted(db.clone(1, direction_t::in)), (std::vector<v_id_t>{3, 4}));
    db.consolidate_all();
    EXPECT_EQ(sorted(db.clone(1, direction_t::in)), (std::vector<v_id_t>{3, 4}));
    std::vector<v_id_t> retained; for (auto v : old) retained.push_back(v);
    EXPECT_EQ(retained, (std::vector<v_id_t>{0, 3}));
  }
  multiscale_store_t db(path / "store", false, true, "absent", options(), 2);
  EXPECT_TRUE(db.clone(0).empty());
  EXPECT_EQ(sorted(db.clone(1, direction_t::in)), (std::vector<v_id_t>{3, 4}));
  EXPECT_EQ(db.clone(2, direction_t::in), (std::vector<v_id_t>{1, 2}));
}
TEST_P(Directed, ColdIncomingGiantConcurrentAndReopen) {
  {
    multiscale_store_t db(path / "store", true, true, "absent", options(), 2, 2, true);
    for (unsigned i = 0; i < 4000; ++i) db.append_vertex();
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) threads.emplace_back([&, t] {
      for (v_id_t v = 1 + t; v < 3500; v += 4) ASSERT_TRUE(db.update(v, 0, INSERT));
    });
    for (auto& thread : threads) thread.join();
    db.consolidate_all(); db.tat().validate();
    EXPECT_TRUE(db.clone(0).empty());
    EXPECT_EQ(db.clone(0, direction_t::in).size(), 3499u);
    auto old = db.read_neighbor(0, direction_t::in);
    ASSERT_TRUE(db.update(1, 0, DELETE));
    ASSERT_TRUE(db.update(3500, 0, INSERT));
    db.consolidate_all();
    EXPECT_EQ(old.size(), 3499u);
    auto in = sorted(db.clone(0, direction_t::in));
    EXPECT_EQ(in.front(), 2u); EXPECT_EQ(in.back(), 3500u);
    std::vector<v_id_t> neighbors{0, 1}; auto v = db.append_vertex(neighbors);
    EXPECT_EQ(sorted(db.clone(v)), neighbors);
    EXPECT_TRUE(db.clone(v, direction_t::in).empty());
    db.consolidate_all();
  }
  multiscale_store_t db(path / "store", false, true, "absent", options(), 2);
  EXPECT_EQ(db.clone(0, direction_t::in).size(), 3500u);
  EXPECT_TRUE(db.clone(0).empty());
  EXPECT_EQ(db.clone(1, direction_t::in), (std::vector<v_id_t>{4000}));
}
TEST_P(Directed, AsymmetricFileAllLayouts) {
  auto file = path / "layouts.graph";
  { std::ofstream out(file); out << "t 8 4\ne 0 1\ne 3 1\ne 2 2\ne 5 6\n"; }
  for (auto layout : {graph_layout_t::id_based, graph_layout_t::kaminpar_gorder,
                       graph_layout_t::louvain_gorder}) {
    BW_GRAPH_LAYOUT = layout;
    auto directory = path / graph_layout_name(layout);
    {
      multiscale_store_t db(directory, true, layout == graph_layout_t::id_based,
                             file.string(), options(), 2);
      EXPECT_TRUE(db.clone(1).empty());
      EXPECT_EQ(sorted(db.clone(1, direction_t::in)), (std::vector<v_id_t>{0, 3}));
      EXPECT_EQ(db.clone(2), (std::vector<v_id_t>{2}));
      EXPECT_EQ(db.clone(2, direction_t::in), (std::vector<v_id_t>{2}));
      EXPECT_TRUE(db.update(7, 1, INSERT)); db.consolidate_all(); db.tat().validate();
    }
    multiscale_store_t db(directory, false, true, "absent", options(), 2);
    EXPECT_EQ(sorted(db.clone(1, direction_t::in)), (std::vector<v_id_t>{0, 3, 7}));
    EXPECT_TRUE(db.clone(1).empty());
  }
}
#ifdef BW_GRAPH_ENABLE_TRANSACTION
TEST_P(Directed, SnapshotBothDirectionsConflictAndWatermark) {
  multiscale_store_t db(path / "store", true, true, "absent", options(), 2, 1, true);
  for (unsigned i = 0; i < 300; ++i) db.append_vertex();
  auto before = db.begin_snapshot();
  std::vector<write_opt_t> writes{{write_ops_type_t::INSERT_EDGE, 0, 299},
                                 {write_ops_type_t::INSERT_EDGE, 1, 298}};
  ASSERT_TRUE(db.commit_edges(before, writes));
  EXPECT_FALSE(db.commit_edges(before, writes));
  auto after = db.begin_snapshot();
  db.consolidate_all();
  EXPECT_TRUE(db.clone_at(0, before).empty());
  EXPECT_TRUE(db.clone_at(299, before, direction_t::in).empty());
  EXPECT_EQ(db.clone_at(299, after, direction_t::in), (std::vector<v_id_t>{0}));
  EXPECT_EQ(db.clone_at(298, after, direction_t::in), (std::vector<v_id_t>{1}));
  db.end_snapshot(before); db.consolidate_all();
  EXPECT_EQ(db.clone_at(299, after, direction_t::in), (std::vector<v_id_t>{0}));
  db.end_snapshot(after);
}
#endif
INSTANTIATE_TEST_SUITE_P(Backends, Directed,
    ::testing::Values(vertex_index_backend_t::array, vertex_index_backend_t::art));
}
