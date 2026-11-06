#ifndef BW_TEST_MULTISCALE_GRAPH_CHECKS_H
#define BW_TEST_MULTISCALE_GRAPH_CHECKS_H
#include "bw_graph/db/db.h"
#include "bw_graph/algo/edge_scan.h"
#include <atomic>
#include <thread>

// Same ground truth and public DB API as the legacy MTR tests, with SMO explicitly off.
template<class Suite>
int run_multiscale_graph_checks(Suite& suite, std::vector<std::vector<v_id_t>> expected,
                                bool id_order) {
  bw_graph::BW_MULTISCALE_ENGINE = true;
  bw_graph::BW_GRAPH_CONSOLIDATION_WORKER_COUNT = 0;
  for (auto& g : bw_graph::BW_MULTISCALE_BUFFER_CONFIG.groups) g = {2, 8};
  auto normalize = [](std::vector<v_id_t> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end()); return v;
  };
  auto check = [&](bw_graph_db_t& db) {
    for (v_id_t v = 0; v < expected.size(); ++v)
      if (normalize(db.read_neighbor_clone(v)) != expected[v]) return false;
    return true;
  };
  {
    bw_graph_db_t db("example", true, true, false, id_order);
    suite.expect(check(db), "multiscale initial adjacency matches ground truth");
    for (v_id_t v = 0; v < expected.size(); ++v) {
      std::vector<v_id_t> base;
      for (auto dst : db.read_neighbor(v)) base.push_back(dst);
      if (normalize(base) != expected[v]) {
        suite.expect(false, "base iterator mismatch"); return suite.finish();
      }
    }
    db.warm_up();
    std::vector<int32_t> distance(expected.size(), -1);
    std::vector<v_id_t> queue{0}; distance[0] = 0;
    for (size_t i = 0; i < queue.size(); ++i)
      for (auto dst : expected[queue[i]]) if (distance[dst] < 0) {
        distance[dst] = distance[queue[i]] + 1; queue.push_back(dst);
      }
    auto bfs = db.parallel_bfs_map(0, 20);
    bool bfs_ok = bfs.size() == queue.size();
    for (const auto& [v, d] : bfs) bfs_ok &= v < distance.size() && distance[v] == int32_t(d);
    suite.expect(bfs_ok, "map BFS distances match independent reference");
    uint64_t edge_count = 0;
    for (const auto& neighbors : expected) edge_count += neighbors.size();
    std::function<uint64_t(v_id_t, v_id_t)> count_edge = [](v_id_t, v_id_t) { return 1; };
    suite.expect(bw_paged_edge_scan<uint64_t>(db, count_edge) == edge_count,
                 "scan visits every base edge exactly once");
    auto before = db.read_neighbor_with_delta(0);
    suite.expect(!before.delta_head.is_valid(), "initial Delta head is invalid");
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (v_id_t t = 0; t < 4; ++t) workers.emplace_back([&, t] {
      try {
        const v_id_t dst = 10 + t;
        for (int i = 0; i < 300; ++i) {
          if (!db.insert_edge(0, dst) || !db.delete_edge(0, dst)) failed = true;
        }
        if (!db.insert_edge(0, dst)) failed = true;
      } catch (...) { failed = true; }
    });
    workers.emplace_back([&] {
      try {
        for (int i = 0; i < 100; ++i) {
          auto snapshot = db.read_neighbor_with_delta(0);
          std::vector<v_id_t> base;
          for (auto dst : snapshot.base_neighbors) base.push_back(dst);
          if (normalize(base) != expected[0]) failed = true;
          size_t length = 0;
          for (auto p = snapshot.delta_head; p.is_valid();) {
            auto rec = db.read_delta_record(p);
            if (rec.first != 0 || ++length > 2404) { failed = true; break; }
            p = {rec.next_page_no, rec.next_record_idx};
          }
        }
      } catch (...) { failed = true; }
    });
    for (auto& w : workers) w.join();
    suite.expect(!failed, "concurrent writes and captured Delta chains are consistent");
    for (v_id_t dst = 10; dst < 14; ++dst) expected[0].push_back(dst);
    expected[0] = normalize(expected[0]);
    db.wait_for_pending_consolidations();
    db.force_switch_new_version();
    db.consolidate_all_pages();
    suite.expect(check(db), "merged adjacency matches all completed updates");
  }
  {
    auto db = bw_graph_db_t::open("example");
    suite.expect(check(*db), "reopened CSR/Delta/Index preserve neighbors");
    db->multiscale->checkpoint();
    const auto sync = db->multiscale->disk().get_sync_count(bw_graph::page_space_t::INDEX);
    db->multiscale->checkpoint();
    suite.expect(sync == db->multiscale->disk().get_sync_count(bw_graph::page_space_t::INDEX),
                 "unchanged resident Index pages are not rewritten");
    db->close();
  }
  return suite.finish();
}
#endif
