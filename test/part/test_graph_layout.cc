#include "bw_graph/common/config.h"
#include "bw_graph/part/louvain.h"
#include "bw_graph/db/db.h"
#include <gtest/gtest.h>
#include <fstream>
#include <set>
#include <unistd.h>

namespace {
using namespace bw_graph;
struct fixture_t {
  std::filesystem::path path, previous = std::filesystem::current_path();
  std::optional<graph_layout_t> layout = BW_GRAPH_LAYOUT;
  bool id = BW_GRAPH_ID_AWARE;
  fixture_t() { char name[] = "/tmp/bw-layout-XXXXXX"; path = mkdtemp(name); }
  ~fixture_t() {
    BW_GRAPH_LAYOUT = layout; BW_GRAPH_ID_AWARE = id;
    std::filesystem::current_path(previous);
    std::error_code error; std::filesystem::remove_all(path, error);
  }
};
TEST(GraphLayout, ExplicitYamlSelectionAndLegacyFallback) {
  fixture_t f;
  auto config = f.path / "layout.yaml";
  for (auto name : {"id-based", "kaminpar-gorder", "louvain-gorder"}) {
    { std::ofstream out(config); out << "storage: {id_aware: true}\npartition: {layout: " << name << "}\n"; }
    load_yaml_config(config.string());
    ASSERT_TRUE(BW_GRAPH_LAYOUT);
    EXPECT_EQ(*BW_GRAPH_LAYOUT, parse_graph_layout(name));
    EXPECT_EQ(BW_GRAPH_ID_AWARE, *BW_GRAPH_LAYOUT == graph_layout_t::id_based);
  }
  { std::ofstream out(config); out << "partition: {layout: typo}\n"; }
  EXPECT_THROW(load_yaml_config(config.string()), std::invalid_argument);
  EXPECT_EQ(BW_GRAPH_LAYOUT, graph_layout_t::louvain_gorder);
  { std::ofstream out(config); out << "storage: {id_aware: true}\n"; }
  load_yaml_config(config.string());
  EXPECT_FALSE(BW_GRAPH_LAYOUT); EXPECT_TRUE(BW_GRAPH_ID_AWARE);
  { std::ofstream out(config); out << "{}\n"; }
  load_yaml_config(config.string());
  EXPECT_FALSE(BW_GRAPH_LAYOUT); EXPECT_FALSE(BW_GRAPH_ID_AWARE);
}
TEST(GraphLayout, LouvainCoversZeroIsolatesAndEntireCommunities) {
  fixture_t f;
  auto file = f.path / "graph";
  { std::ofstream out(file); out << "t 12 20\n";
    for (uint32_t base : {0, 5}) for (uint32_t u = base; u < base + 5; ++u)
      for (uint32_t v = u + 1; v < base + 5; ++v) out << "e " << u << ' ' << v << '\n'; }
  mem_sub_com_t graph(file.string(), true, LOAD_BUFFER_SIZE);
  auto parts = compute_louvain_communities(graph);
  std::vector<size_t> owner(12, SIZE_MAX);
  for (size_t c = 0; c < parts.size(); ++c) for (auto v : parts[c]) {
    ASSERT_LT(v, 12u); ASSERT_EQ(owner[v], SIZE_MAX); owner[v] = c;
  }
  for (auto c : owner) EXPECT_NE(c, SIZE_MAX);
  for (uint32_t base : {0, 5}) for (uint32_t v = base; v < base + 5; ++v)
    EXPECT_EQ(owner[v], owner[base]);
  EXPECT_NE(owner[0], owner[5]); EXPECT_NE(owner[10], owner[11]);
  EXPECT_GT(last_louvain_build.threads, 0);
  { std::ofstream out(file); out << "t 4 0\n"; }
  mem_sub_com_t empty(file.string(), true, LOAD_BUFFER_SIZE);
  auto isolated = compute_louvain_communities(empty);
  EXPECT_EQ(isolated.size(), 4u);
}
TEST(GraphLayout, AllLayoutsNativeWritesSmoAndReopenFromYaml) {
  fixture_t f;
  std::filesystem::create_directory(f.path / "data");
  { std::ofstream out(f.path / "data/layout.graph"); out << "t 1400 1200\n";
    for (uint32_t v = 1; v <= 1200; ++v) out << "e 0 " << v << '\n'; }
  std::filesystem::current_path(f.path);
  for (auto name : {"id-based", "kaminpar-gorder", "louvain-gorder"}) {
    auto config = f.path / "layout.yaml";
    auto directory = (f.path / name).string();
    { std::ofstream out(config);
      out << "storage: {engine: multiscale, id_aware: true, csr_prefix: " << directory
          << ", delta_prefix: " << directory << ", meta_prefix: " << directory << "}\n"
          << "partition: {layout: " << name << ", giant_degree_bound: 1024, coarsening_weight: 4096}\n"
          << "thread: {consolidation_workers: 2}\n"
          << "index: {vertex_index: " << vertex_index_backend_name(BW_VERTEX_INDEX_BACKEND)
          << "}\n"; }
    load_yaml_config(config.string());
    {
      // Even a conflicting legacy constructor flag cannot override explicit YAML.
      bw_graph_db_t db("layout", true, true, false, true);
      // Hundreds of singleton communities must share partial pages, rather
      // than each reserving a mostly empty 4 KiB page.
      EXPECT_LT(db.multiscale->tat().stats().leaves, 30u);
      EXPECT_EQ(db.read_neighbor_clone(0).size(), 1200u);
      ASSERT_TRUE(db.insert_edge(0, 1300));
      ASSERT_TRUE(db.insert_edge(1300, 1301));
      db.consolidate_all_pages();
      db.multiscale->tat().validate();
      EXPECT_EQ(db.read_neighbor_clone(0).size(), 1201u);
      db.close();
    }
    {
      bw_graph_db_t db("layout", false, false, false, true);
      EXPECT_EQ(db.read_neighbor_clone(0).size(), 1201u);
      EXPECT_EQ(db.read_neighbor_clone(1300), (std::vector<v_id_t>{1301}));
      db.multiscale->tat().validate();
      db.close();
    }
  }
}
} // namespace
