#include "bw_graph/db/multiscale_store.h"
#include <gtest/gtest.h>
#include "bw_graph/part/reorder.h"
#include <fstream>
#include <thread>
#include <set>
#include <unistd.h>

using namespace bw_graph;
namespace {
struct fixture_t {
  std::filesystem::path path;
  fixture_t() {
    char name[] = "/tmp/bw-engine-test-XXXXXX";
    path = mkdtemp(name);
  }
  ~fixture_t() { std::error_code e; std::filesystem::remove_all(path, e); }
};
multiscale_buf_options_t budget(size_t n = 16) {
  multiscale_buf_options_t o;
  for (auto& g : o.groups) g = {2, n};
  return o;
}
std::vector<v_id_t> sorted(std::vector<v_id_t> v) {
  std::sort(v.begin(), v.end()); return v;
}
TEST(MultiscaleGraph, AllScalesStreamingDeltaConcurrencyReopen) {
  fixture_t f;
  const auto file = f.path / "graph";
  {
    std::ofstream out(file);
    out << "t 540100 1220000\n";
    for (uint32_t dst = 4; dst < 530004; ++dst) out << "e 0 " << dst << "\ne " << dst << " 0\n";
    for (uint32_t dst = 4; dst < 70004; ++dst) out << "e 1 " << dst << "\ne " << dst << " 1\n";
    for (uint32_t dst = 4; dst < 10004; ++dst) out << "e 2 " << dst << "\ne " << dst << " 2\n";
  }
  const auto store = f.path / "store";
  {
    multiscale_store_t db(store, true, true, file.string(), budget());
    for (size_t c = 0; c < 4; ++c)
      EXPECT_GT(db.disk().get_page_count(static_cast<page_space_t>(c)), 0);
    const std::array<size_t, 3> degrees{530000, 70000, 10000};
    for (size_t v = 0; v < degrees.size(); ++v) {
      auto incoming = db.read_neighbor(v, direction_t::in);
      EXPECT_EQ(incoming.size(), degrees[v]);
      uint64_t in_sum = 0;
      for (auto src : incoming) in_sum += src;
      EXPECT_EQ(in_sum, degrees[v] * (degrees[v] + 7) / 2);
      auto range = db.read_neighbor(v);
      EXPECT_EQ(range.size(), degrees[v]);
      uint64_t count = 0, sum = 0;
      for (auto dst : range) { ++count; sum += dst; }
      EXPECT_EQ(count, degrees[v]);
      EXPECT_EQ(sum, degrees[v] * (degrees[v] + 7) / 2);
      auto chunks = db.read_neighbor(v);
      size_t chunk_count = 0, chunk_edges = 0;
      uint64_t chunk_sum = 0;
      chunks.visit_chunks([&](auto bytes) {
        ++chunk_count;
        chunk_edges += bytes.size() / 4;
        for (size_t i = 0; i < bytes.size(); i += 4) {
          v_id_t dst; std::memcpy(&dst, bytes.data() + i, 4); chunk_sum += dst;
        }
        return false;
      });
      EXPECT_GT(chunk_count, 0u);
      EXPECT_EQ(chunk_edges, count);
      EXPECT_EQ(chunk_sum, sum);
    }
    EXPECT_TRUE(db.update(0, 4, DELETE));
    EXPECT_TRUE(db.update(0, 530010, INSERT));
    const auto captured = db.delta_head(0);
    EXPECT_TRUE(db.update(0, 530011, INSERT));
    size_t length = 0;
    for (auto p = captured; p != UINT64_MAX;) {
      auto rec = db.read_delta(p); ++length;
      p = (uint64_t(rec.next_page_no) << 32) | rec.next_record_idx;
    }
    EXPECT_EQ(length, 2);
    EXPECT_EQ(db.read_neighbor(0).size(), 530000); // Base is unchanged by deltas.
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (uint32_t t = 0; t < 4; ++t) workers.emplace_back([&, t] {
      try {
        for (uint32_t i = 0; i < 150; ++i) {
          auto dst = 531000 + t * 150 + i;
          if (!db.update(0, dst, INSERT) || !db.update(0, dst, DELETE) ||
              !db.update(0, dst, INSERT)) failed = true;
        }
      } catch (...) { failed = true; }
    });
    workers.emplace_back([&] {
      try {
        for (size_t i = 0; i < 40; ++i) {
          auto head = db.delta_head(0);
          while (head != UINT64_MAX) {
            auto rec = db.read_delta(head);
            if (rec.first != 0) failed = true;
            head = (uint64_t(rec.next_page_no) << 32) | rec.next_record_idx;
          }
          if (db.read_neighbor(1).size() != 70000) failed = true;
        }
      } catch (...) { failed = true; }
    });
    for (auto& w : workers) w.join();
    EXPECT_FALSE(failed);
    auto result = sorted(db.clone(0));
    EXPECT_EQ(result.size(), 530601);
    EXPECT_FALSE(std::binary_search(result.begin(), result.end(), 4));
    EXPECT_TRUE(std::binary_search(result.begin(), result.end(), 531599));
    db.checkpoint();
    const auto sync = db.disk().get_sync_count(page_space_t::INDEX);
    db.checkpoint();
    EXPECT_EQ(sync, db.disk().get_sync_count(page_space_t::INDEX));
    db.close();
  }
  {
    multiscale_store_t db(store, false, true, "", budget());
    auto result = sorted(db.clone(0));
    EXPECT_EQ(result.size(), 530601);
    EXPECT_FALSE(std::binary_search(result.begin(), result.end(), 4));
    EXPECT_TRUE(std::binary_search(result.begin(), result.end(), 531599));
    EXPECT_TRUE(db.update(0, 531599, DELETE));
    EXPECT_EQ(db.clone(0).size(), 530600);
    db.close();
  }
}
TEST(MultiscaleGraph, PartitionOrderAndBudgetFailure) {
  fixture_t f;
  auto file = f.path / "graph";
  { std::ofstream out(file); out << "t 100 198\n";
    for (int i = 1; i < 100; ++i) out << "e " << i - 1 << ' ' << i << "\ne " << i << ' ' << i - 1 << '\n'; }
  multiscale_store_t db(f.path / "store", true, false, file.string(), budget(2));
  for (uint32_t v = 0; v < 100; ++v) {
    std::vector<v_id_t> want;
    if (v) want.push_back(v - 1);
    if (v < 99) want.push_back(v + 1);
    EXPECT_EQ(sorted(db.clone(v)), want);
  }
  db.warm_up();
  EXPECT_TRUE(db.insert_vertex(100));
  EXPECT_TRUE(db.update(100, 0, INSERT));
  EXPECT_EQ(db.clone(100), std::vector<v_id_t>{0});
  EXPECT_TRUE(db.delete_vertex(100));
  EXPECT_TRUE(db.clone(100).empty());
  EXPECT_FALSE(db.delete_vertex(100));
  EXPECT_TRUE(db.insert_vertex(100));
  EXPECT_FALSE(db.insert_vertex(100));
  EXPECT_EQ(db.clone(100), std::vector<v_id_t>{0});
}
}

TEST(MultiscaleGraph, TinyCacheStreamingGroupedReadsAndConcurrentVertexAppend) {
  fixture_t f;
  auto file = f.path / "graph";
  { std::ofstream out(file); out << "t 15000 29998\n";
    for (int i = 1; i < 15000; ++i) out << "e " << i - 1 << ' ' << i << "\ne " << i << ' ' << i - 1 << '\n'; }
  multiscale_store_t db(f.path / "store", true, true, file.string(), budget(2));
  ASSERT_FALSE(db.base_fits());
  db.warm_up();
  for (size_t pass = 0; pass < 2; ++pass) {
    const auto order = db.scan_order();
    const auto groups = db.scan_groups();
    uint64_t edges = 0;
    for (size_t g = 0; g + 1 < groups.size(); ++g)
      db.visit_base_group(order.subspan(groups[g], groups[g + 1] - groups[g]),
          [&](v_id_t src, auto neighbors) {
        for (auto dst : neighbors) { EXPECT_EQ(std::max(src, dst) - std::min(src, dst), 1u); ++edges; }
      });
    EXPECT_EQ(edges, 29998u);
  }
  std::array<std::vector<v_id_t>, 4> ids;
  std::vector<std::thread> workers;
  for (size_t t = 0; t < ids.size(); ++t) workers.emplace_back([&, t] {
    for (size_t i = 0; i < 80; ++i) ids[t].push_back(db.append_vertex());
  });
  std::atomic<bool> failed{false};
  workers.emplace_back([&] {
    try {
      for (size_t i = 0; i < 100; ++i) {
        auto n = db.vertex_count();
        if (n > 15000 && !db.read_neighbor(n - 1).empty()) failed = true;
        if (db.read_neighbor(0).size() != 1) failed = true;
      }
    } catch (...) { failed = true; }
  });
  for (auto& w : workers) w.join();
  EXPECT_FALSE(failed);
  std::set<v_id_t> unique;
  for (auto& values : ids) unique.insert(values.begin(), values.end());
  EXPECT_EQ(unique.size(), 320u);
  EXPECT_EQ(db.vertex_count(), 15320u);
  EXPECT_TRUE(db.delete_vertex(1));
  EXPECT_EQ(db.degree(1), 0u);
  EXPECT_TRUE(db.read_neighbor(1).empty());
  EXPECT_GT(db.pool().stats(page_space_t::CSR_4K).evictions, 0u);
  db.close();
  multiscale_store_t reopened(f.path / "store", false, true, "", budget(2));
  EXPECT_EQ(reopened.vertex_count(), 15320u);
  EXPECT_EQ(reopened.degree(1), 0u);
}

TEST(MultiscaleGraph, RejectsCorruptPageIdentityAndInfeasibleGiantBudget) {
  fixture_t f;
  auto file = f.path / "graph";
  { std::ofstream out(file); out << "t 10001 10000\n";
    for (int i = 1; i <= 10000; ++i) out << "e 0 " << i << '\n'; }
  const auto store = f.path / "store";
  page_id_t page;
  {
    multiscale_store_t db(store, true, true, file.string(), budget(4));
    page = db.location(0).first.page;
    EXPECT_GT(static_cast<unsigned>(page.space()), 0u);
    db.close();
  }
  {
    auto options = budget(4); options.groups[1] = {0, 0};
    multiscale_store_t db(store, false, true, "", options);
    EXPECT_THROW(db.warm_up(), std::runtime_error);
    db.close();
  }
  {
    multiscale_disk_options_t options; options.index_page_size = 16384;
    multiscale_disk_manager_t disk(store, options);
    auto image = disk.read_page(page);
    image.bytes()[12] ^= std::byte{1};
    disk.write_page(image); disk.flush();
  }
  multiscale_store_t db(store, false, true, "", budget(4));
  EXPECT_THROW((void)db.read_neighbor(0), std::runtime_error);
}

TEST(MultiscaleGraph, HotRangesShrinkAndReadDominantRangesCoalesce) {
  fixture_t f;
  const auto file = f.path / "graph";
  { std::ofstream out(file); out << "t 80000 70000\n";
    for (unsigned v = 1; v <= 70000; ++v) out << "e 0 " << v << '\n'; }
  multiscale_store_t db(f.path / "store", true, true, file.string(), budget(16), 1, 10000);
  EXPECT_EQ(db.read_neighbor(0).page_count(page_space_t::CSR_256K), 1u);
  // A single hot key must count as repeated writes, not just one distinct change.
  for (unsigned r = 0; r < 20; ++r) {
    ASSERT_TRUE(db.update(0, 12345, DELETE));
    ASSERT_TRUE(db.update(0, 12345, INSERT));
  }
  db.consolidate_all();
  EXPECT_EQ(db.read_neighbor(0).page_count(page_space_t::CSR_256K), 0u);
  EXPECT_EQ(db.read_neighbor(0).size(), 70000u);
  for (v_id_t dst = 1; dst <= 1000; ++dst) ASSERT_TRUE(db.update(0, dst, DELETE));
  db.consolidate_all();
  EXPECT_EQ(db.read_neighbor(0).page_count(page_space_t::CSR_256K), 0u);
  EXPECT_EQ(db.read_neighbor(0).size(), 69000u);
  // Actual full scans provide the read signal, not a synthetic counter override.
  for (unsigned r = 0; r < 100; ++r) {
    auto range = db.read_neighbor(0); uint64_t count = 0;
    for ([[maybe_unused]] auto dst : range) ++count;
    ASSERT_EQ(count, 69000u);
  }
  ASSERT_TRUE(db.update(0, 1, INSERT));
  db.consolidate_all();
  EXPECT_EQ(db.read_neighbor(0).page_count(page_space_t::CSR_256K), 1u);
  EXPECT_EQ(db.clone(0).size(), 69001u);
}

TEST(MultiscaleGraph, SparseGiantChangesDeferUntilRepeatedReadsJustifyRewrite) {
  fixture_t f;
  const auto file = f.path / "graph";
  {
    std::ofstream out(file);
    out << "t 12000 10000\n";
    for (unsigned v = 1; v <= 10000; ++v) out << "e 0 " << v << '\n';
  }
  multiscale_store_t db(f.path / "store", true, true, file.string(), budget(16), 1, 1);
  ASSERT_TRUE(db.update(0, 11000, INSERT));
  db.wait_for_smo();
  EXPECT_GT(db.smo_stats().deferred, 0u);
  EXPECT_EQ(db.read_neighbor(0).size(), 10000u);
  EXPECT_EQ(db.clone(0).size(), 10001u);
  for (unsigned r = 0; r < 1024; ++r) {
    auto range = db.read_neighbor(0);
    size_t count = 0;
    for ([[maybe_unused]] auto dst : range) ++count;
    ASSERT_EQ(count, 10000u);
  }
  ASSERT_TRUE(db.update(0, 11001, INSERT));
  db.wait_for_smo();
  auto merged = db.read_neighbor(0);
  EXPECT_EQ(merged.size(), 10002u);
  EXPECT_EQ(merged.delta_head(), merged.delta_stop());
}

TEST(MultiscaleGraph, SmoPreservesContiguousTwoMegabyteRunsAndBorrowedHistory) {
  fixture_t f;
  const auto file = f.path / "graph", store = f.path / "store";
  constexpr uint32_t degree = 1050000;
  {
    std::ofstream out(file);
    out << "t " << degree + 2 << ' ' << degree << '\n';
    for (uint32_t v = 1; v <= degree; ++v) out << "e 0 " << v << '\n';
  }
  {
    multiscale_store_t db(store, true, true, file.string(), budget(2), 1, 10000);
    ASSERT_EQ(db.location(0).first.page.space(), page_space_t::CSR_2M);
    ASSERT_EQ(db.location(0).first.count, 2u);
    {
      auto old = db.read_neighbor(0);
      ASSERT_TRUE(db.update(0, 1, DELETE));
      ASSERT_TRUE(db.update(0, degree + 1, INSERT));
      db.consolidate_all();
      uint64_t count = 0, sum = 0;
      for (auto dst : old) { ++count; sum += dst; }
      EXPECT_EQ(count, degree);
      EXPECT_EQ(sum, uint64_t(degree) * (degree + 1) / 2);
      auto current = db.read_neighbor(0);
      EXPECT_EQ(current.size(), degree);
      EXPECT_EQ(current.delta_head(), current.delta_stop());
    }
    db.close();
  }
  multiscale_store_t reopened(store, false, true, "", budget(1), 1, 10000);
  auto values = sorted(reopened.clone(0));
  ASSERT_EQ(values.size(), degree);
  EXPECT_EQ(values.front(), 2u);
  EXPECT_EQ(values.back(), degree + 1);
  EXPECT_EQ(std::adjacent_find(values.begin(), values.end()), values.end());
}

TEST(MultiscaleTat, RecursiveSplitPersistenceAndMembership) {
  multiscale_tat_t tat(4);
  auto edges = [](v_id_t v) { return std::vector<v_id_t>{v ^ 1u}; };
  std::vector<multiscale_tat_t::leaf_t> initial;
  for (uint32_t i = 0; i < 4; ++i)
    initial.push_back({page_id_t::make(page_space_t::CSR_4K, i), {i * 2, i * 2 + 1}});
  tat.build(initial, edges);
  for (uint32_t i = 0; i < 4; ++i) {
    tat.replace(initial[i].page,
        {{page_id_t::make(page_space_t::CSR_4K, 10 + 2 * i), {i * 2}},
         {page_id_t::make(page_space_t::CSR_4K, 11 + 2 * i), {i * 2 + 1}}}, edges);
    tat.validate();
  }
  for (uint32_t i = 8; i < 90; ++i) {
    tat.append({page_id_t::make(page_space_t::CSR_4K, 100 + i), {i}}, edges);
    tat.validate();
  }
  EXPECT_GE(tat.stats().height, 4u);
  EXPECT_GT(tat.stats().splits, 2u);
  EXPECT_EQ(tat.stats().leaves, 90u);
  std::vector<std::byte> bytes;
  tat.encode(bytes);
  multiscale_tat_t reopened;
  reopened.decode(bytes);
  reopened.validate();
  EXPECT_EQ(reopened.stats().height, tat.stats().height);
  EXPECT_EQ(reopened.stats().leaves, tat.stats().leaves);
  auto before = tat.leaves(), after = reopened.leaves();
  ASSERT_EQ(before.size(), after.size());
  for (size_t i = 0; i < before.size(); ++i) {
    EXPECT_EQ(before[i].page, after[i].page);
    EXPECT_EQ(before[i].vertices, after[i].vertices);
  }
  bytes.pop_back();
  EXPECT_THROW(reopened.decode(bytes), std::runtime_error);
}

TEST(MultiscaleGraph, GorderSequentialLeafSplitAndTatReopen) {
  fixture_t f;
  auto file = f.path / "graph";
  { std::ofstream out(file); out << "t 40 80\n";
    for (uint32_t i = 0; i < 40; ++i) out << "e " << i << ' ' << (i + 2) % 40 << "\ne " << (i + 2) % 40 << ' ' << i << '\n'; }
  auto options = budget(4);
  std::vector<std::vector<v_id_t>> expected(40);
  size_t leaves = 0;
  {
    // Disable automatic scheduling by threshold, not SMO. A single forced
    // whole-leaf split makes its physical Gorder sequence directly testable.
    multiscale_store_t db(f.path / "store", true, false, file.string(), options, 2, 100000);
    auto original = db.tat().leaves();
    ASSERT_EQ(original.size(), 1u);
    auto borrowed = db.read_neighbor(0);
    adj_map_t graph;
    for (auto v : original[0].vertices) {
      for (v_id_t dst = 0; dst < 40; ++dst) if (dst != v) {
        ASSERT_TRUE(db.update(v, dst, INSERT));
        expected[v].push_back(dst);
      }
      graph.emplace(v, expected[v]);
    }
    const auto want_order = gorder_reordering_optimized(graph);
    db.consolidate_all();
    db.tat().validate();
    auto pages = db.tat().leaves();
    leaves = pages.size();
    ASSERT_GT(leaves, 1u);
    std::vector<v_id_t> actual_order;
    for (const auto& leaf : pages) {
      auto guard = db.pool().fetch_read(leaf.page);
      uint32_t count;
      std::memcpy(&count, guard.bytes().data() + 16, 4);
      ASSERT_EQ(count, leaf.vertices.size());
      for (size_t i = 0; i < count; ++i) {
        uint32_t v;
        std::memcpy(&v, guard.bytes().data() + 40 + i * 32, 4);
        EXPECT_EQ(v, leaf.vertices[i]);
        actual_order.push_back(v);
      }
    }
    EXPECT_EQ(actual_order, want_order);
    EXPECT_EQ(borrowed.size(), 2u);
    size_t old_count = 0;
    for (auto dst : borrowed) { EXPECT_TRUE(dst == 2 || dst == 38); ++old_count; }
    EXPECT_EQ(old_count, 2u);
    for (uint32_t v = 0; v < 40; ++v) EXPECT_EQ(sorted(db.clone(v)), expected[v]);
    auto replacement_count = db.tat().stats().replacements;
    db.consolidate_all();
    EXPECT_EQ(db.tat().stats().replacements, replacement_count);
    // Release borrowed guard before close.
  }
  {
    multiscale_store_t db(f.path / "store", false, false, "", options, 2, 1);
    db.tat().validate();
    EXPECT_EQ(db.tat().stats().leaves, leaves);
    for (uint32_t v = 0; v < 40; ++v) EXPECT_EQ(sorted(db.clone(v)), expected[v]);
    ASSERT_TRUE(db.update(0, 1, DELETE));
    db.consolidate_all();
    db.tat().validate();
    expected[0].erase(expected[0].begin());
    EXPECT_EQ(sorted(db.clone(0)), expected[0]);
  }
}
