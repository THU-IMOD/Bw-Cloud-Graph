#include "bw_graph/common/config.h"
#include "bw_graph/service/graph_service.h"

#include <atomic>
#include <barrier>
#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <thread>
#include <unistd.h>
using bw_graph::service::graph_service_t;
namespace {
class GraphService : public ::testing::TestWithParam<bw_graph::vertex_index_backend_t> {
protected:
  std::filesystem::path root;
  bw_graph::vertex_index_backend_t saved;
  void SetUp() override {
    saved = bw_graph::BW_VERTEX_INDEX_BACKEND;
    bw_graph::BW_VERTEX_INDEX_BACKEND = GetParam();
    char pattern[] = "/tmp/bw-jni-service-XXXXXX";
    root = mkdtemp(pattern);
  }
  void TearDown() override {
    bw_graph::BW_VERTEX_INDEX_BACKEND = saved;
    std::filesystem::remove_all(root);
  }
};
TEST_P(GraphService, BinaryIdsPropertiesDirectedDeleteAndReopen) {
  graph_service_t api;
  auto h = api.open(root, "g");
  EXPECT_TRUE(api.all(h, false).empty());
  EXPECT_EQ(api.components(h, false), (std::vector<uint64_t>{0}));
  EXPECT_EQ(api.communities(h), (std::vector<uint64_t>{0}));
  auto same = api.open(root, "g");
  const std::string binary("\0uuid\xff", 6), value("x\0y", 3);
  auto a = api.create_vertex(h, binary, value);
  auto b = api.create_vertex(h, "b", "B");
  auto c = api.create_vertex(h, "c", "C");
  EXPECT_EQ(api.find(same, false, binary), a);
  EXPECT_EQ(api.external_id(h, false, a), binary);
  EXPECT_EQ(api.get(h, false, a), value);
  EXPECT_THROW(api.create_edge(h, "missing", a, 99999, ""), std::out_of_range);
  auto ab = api.create_edge(h, "ab", a, b, value);
  auto ba = api.create_edge(h, "ba", b, a, "");
  auto bc = api.create_edge(h, "bc", b, c, "");
  auto aa = api.create_edge(h, "aa", a, a, "");
  EXPECT_EQ(api.incident(h, a, 0), (std::vector<uint64_t>{aa, ab}));
  EXPECT_EQ(api.incident(h, a, 1), (std::vector<uint64_t>{aa, ba}));
  EXPECT_EQ(api.incident(h, a, 2).size(), 3u);
  EXPECT_EQ(api.bfs(h, a), (std::vector<uint64_t>{a, 0, b, 1, c, 2}));
  EXPECT_EQ(api.components(h, false)[0], 1u);
  EXPECT_EQ(api.components(h, true)[0], 2u);
  EXPECT_THROW(api.create_edge(h, "parallel", a, b, ""), std::invalid_argument);
  api.put(h, true, ab, "updated");
  api.close(h);
  EXPECT_EQ(api.get(same, true, ab), "updated");
  EXPECT_THROW(api.get(h, false, a), std::invalid_argument);
  api.close(same);
  h = api.open(root, "g");
  EXPECT_EQ(api.get(h, true, ab), "updated");
  api.remove(h, false, b);
  EXPECT_EQ(api.find(h, true, "ab"), -1);
  EXPECT_EQ(api.find(h, true, "ba"), -1);
  EXPECT_EQ(api.find(h, true, "bc"), -1);
  EXPECT_EQ(api.all(h, true), (std::vector<uint64_t>{aa}));
  EXPECT_TRUE(api.incident(h, c, 1).empty());
  api.close(h);
  h = api.open(root, "g");
  EXPECT_EQ(api.find(h, false, "b"), -1);
  EXPECT_EQ(api.all(h, false), (std::vector<uint64_t>{a, c}));
  auto community = api.communities(h);
  EXPECT_EQ(community, (std::vector<uint64_t>{2, a, 0, c, 1}));
  EXPECT_EQ(api.all(h, true), (std::vector<uint64_t>{aa}));
  api.close(h);
  (void) bc;
}
TEST_P(GraphService, ParallelPointOperationsAndDuplicateIdRace) {
  graph_service_t api;
  auto h = api.open(root, "parallel");
  constexpr int workers = 8, rounds = 80;
  std::vector<uint64_t> vertices;
  for (int i = 0; i < workers * 2; ++i)
    vertices.push_back(api.create_vertex(h, std::to_string(i), "initial"));
  std::barrier start(workers);
  std::atomic<int> errors{0}, winners{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < workers; ++t)
    threads.emplace_back([&, t] {
      start.arrive_and_wait();
      try {
        auto src = vertices[2 * t], dst = vertices[2 * t + 1];
        auto e = api.create_edge(h, "edge" + std::to_string(t), src, dst, "initial");
        for (int i = 0; i < rounds; ++i) {
          auto value = std::to_string(i);
          api.put(h, false, src, value);
          api.put(h, true, e, value);
          if (api.get(h, false, src) != value || api.get(h, true, e) != value ||
              api.find(h, false, std::to_string(2 * t)) != int64_t(src) ||
              api.incident(h, dst, 1) != std::vector<uint64_t>{e})
            ++errors;
          // Hot shared property exercises serialization without requiring a value order.
          api.put(h, false, vertices[1], value);
          (void) api.get(h, false, vertices[1]);
        }
        try {
          api.create_edge(h, "contended", dst, src, "hot");
          ++winners;
        } catch (const std::invalid_argument&) {
        }
      } catch (...) {
        ++errors;
      }
    });
  for (auto& thread : threads)
    thread.join();
  EXPECT_EQ(errors, 0);
  EXPECT_EQ(winners, 1);
  EXPECT_EQ(api.all(h, true).size(), workers + 1);
  api.close(h);
  h = api.open(root, "parallel");
  EXPECT_EQ(api.all(h, true).size(), workers + 1);
  api.close(h);
}
TEST_P(GraphService, IndependentGraphsAndInvalidNames) {
  graph_service_t api;
  EXPECT_THROW(api.open(root, "../escape"), std::invalid_argument);
  auto a = api.open(root, "a"), b = api.open(root, "b");
  auto v = api.create_vertex(a, "id", "A");
  api.create_vertex(b, "id", "B");
  api.close(a);
  EXPECT_EQ(api.get(b, false, v), "B");
  api.close(b);
  std::filesystem::create_directory_symlink(root / "a", root / "link");
  EXPECT_THROW(api.open(root, "link"), std::invalid_argument);
}
TEST_P(GraphService, CloseDrainsConcurrentRequests) {
  graph_service_t api;
  auto h = api.open(root, "close");
  auto v = api.create_vertex(h, "id", "value");
  std::barrier ready(5);
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&] {
      ready.arrive_and_wait();
      for (int i = 0; i < 2000; ++i) {
        try {
          if (api.find(h, false, "id") != int64_t(v) || api.get(h, false, v) != "value")
            ++errors;
        } catch (const std::invalid_argument&) {
          break;
        } // Registry token retired.
        catch (const std::runtime_error&) {
          break;
        } // Request arrived before close, ran after it.
        catch (...) {
          ++errors;
          break;
        }
      }
    });
  ready.arrive_and_wait();
  api.close(h);
  for (auto& thread : threads)
    thread.join();
  EXPECT_EQ(errors, 0);
  auto reopened = api.open(root, "close");
  EXPECT_EQ(api.get(reopened, false, v), "value");
  api.close(reopened);
}
TEST_P(GraphService, RejectUncleanStoreWithoutRebuild) {
  graph_service_t api;
  auto h = api.open(root, "unclean");
  api.create_vertex(h, "keep", "data");
  api.close(h);
  {
    rocksdb::DB* raw = nullptr;
    ASSERT_TRUE(rocksdb::DB::Open({}, (root / "unclean" / "metadata").string(), &raw).ok());
    std::unique_ptr<rocksdb::DB> db(raw);
    ASSERT_TRUE(db->Put({}, "state", "open").ok());
  }
  EXPECT_THROW(api.open(root, "unclean"), std::runtime_error);
  // An unrelated graph remains available after a failed open.
  h = api.open(root, "other");
  EXPECT_EQ(api.create_vertex(h, "id", "data"), 0u);
  api.close(h);
}
TEST_P(GraphService, PublishedLookupDuringEraseAndReinsert) {
  graph_service_t api;
  auto h = api.open(root, "publication");
  auto a = api.create_vertex(h, "a", "");
  auto b = api.create_vertex(h, "b", "");
  const auto expected = (a << 32) | b;
  std::atomic<bool> done{false};
  std::atomic<int> errors{0}, reads{0};
  std::barrier start(4);
  std::vector<std::thread> readers;
  for (int t = 0; t < 3; ++t)
    readers.emplace_back([&] {
      start.arrive_and_wait();
      while (!done.load()) {
        try {
          auto edge = api.find(h, true, "same-id");
          if (edge != -1 && edge != int64_t(expected))
            ++errors;
          if (edge != -1) {
            try {
              if (api.get(h, true, edge) != "published")
                ++errors;
            } catch (const std::out_of_range&) {
            } // Deleted between the two calls.
          }
          ++reads;
        } catch (...) {
          ++errors;
          break;
        }
      }
    });
  start.arrive_and_wait();
  for (int i = 0; i < 500; ++i) {
    EXPECT_EQ(api.create_edge(h, "same-id", a, b, "published"), expected);
    api.remove(h, true, expected);
  }
  done = true;
  for (auto& reader : readers)
    reader.join();
  EXPECT_EQ(errors, 0);
  EXPECT_GT(reads, 0);
  EXPECT_EQ(api.find(h, true, "same-id"), -1);
  api.close(h);
}
INSTANTIATE_TEST_SUITE_P(Backends, GraphService,
                         ::testing::Values(bw_graph::vertex_index_backend_t::array,
                                           bw_graph::vertex_index_backend_t::art));
} // namespace
