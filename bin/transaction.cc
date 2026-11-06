#include "bw_graph/common/config.h"
#include "bw_graph/db/db.h"
#if defined(BW_GRAPH_ENABLE_TRANSACTION)
#include "bw_graph/db/txn.h"
#endif
#include <cassert>
#include <iostream>

#ifdef BW_GRAPH_ENABLE_TRANSACTION

// ─── helpers ─────────────────────────────────────────────────────────────────

static bool has_neighbor(const std::vector<v_id_t>& nbrs, v_id_t target) {
  for (v_id_t n : nbrs) {
    if (n == target)
      return true;
  }
  return false;
}

static void check(bool condition, const char* msg) {
  if (condition) {
    std::cout << "PASS: " << msg << std::endl;
  } else {
    std::cerr << "FAIL: " << msg << std::endl;
  }
}

// ─── Test 1: basic snapshot isolation ────────────────────────────────────────

static void run_smoke_test(bw_graph_db_t& db) {
  std::cout << "\n=== Transaction Smoke Test ===" << std::endl;

  auto t1 = db.begin_txn();
  auto t2 = db.begin_txn(); // snapshot predates T1's commit

  t1->insert_edge(1, 2);

  check(!has_neighbor(t2->get_neighbors(1), 2), "T2 does not see T1's edge before T1 commits");

  bool ok = t1->commit();
  assert(ok);
  std::cout << "T1 committed" << std::endl;

  check(!has_neighbor(t2->get_neighbors(1), 2),
        "T2 still does not see T1's edge (snapshot predates commit)");
  t2->abort();

  auto t3 = db.begin_txn();
  check(has_neighbor(t3->get_neighbors(1), 2), "T3 (post-commit) sees T1's committed edge");
  t3->abort();

  std::cout << "Watermark: " << db.get_txn_manager()->get_watermark() << std::endl;
  std::cout << "=== Smoke test complete ===" << std::endl;
}

// ─── Test 2: delete visibility ────────────────────────────────────────────────

static void run_delete_test(bw_graph_db_t& db) {
  std::cout << "\n=== Delete Visibility Test ===" << std::endl;

  // Insert edge 1→3 so there is something to delete.
  {
    auto ti = db.begin_txn();
    ti->insert_edge(1, 3);
    assert(ti->commit());
  }

  auto t_snap = db.begin_txn(); // snapshot includes 1→3

  // Delete the edge in a new transaction.
  {
    auto td = db.begin_txn();
    td->delete_edge(1, 3);
    assert(td->commit());
    std::cout << "Delete committed" << std::endl;
  }

  // t_snap predates the delete — it should still see 1→3.
  check(has_neighbor(t_snap->get_neighbors(1), 3), "snapshot before delete still sees edge 1→3");
  t_snap->abort();

  // New transaction started after delete — must NOT see 1→3.
  auto t_post = db.begin_txn();
  check(!has_neighbor(t_post->get_neighbors(1), 3),
        "post-delete transaction does not see edge 1→3");
  t_post->abort();

  std::cout << "=== Delete test complete ===" << std::endl;
}

// ─── Test 3: multiple concurrent writers, ordered visibility ─────────────────

static void run_concurrent_writer_test(bw_graph_db_t& db) {
  std::cout << "\n=== Concurrent Writer Ordering Test ===" << std::endl;

  // T_a and T_b are both open; each inserts a different edge from vertex 2.
  auto ta = db.begin_txn();
  auto tb = db.begin_txn();

  ta->insert_edge(2, 10);
  tb->insert_edge(2, 20);

  // Neither should see the other's edge yet.
  check(!has_neighbor(ta->get_neighbors(2), 20), "Ta does not see Tb's uncommitted edge");
  check(!has_neighbor(tb->get_neighbors(2), 10), "Tb does not see Ta's uncommitted edge");

  // Ta commits first.
  assert(ta->commit());
  std::cout << "Ta committed (edge 2→10)" << std::endl;

  // Tb still cannot see Ta's edge (Tb's read_ts < Ta's commit_ts).
  check(!has_neighbor(tb->get_neighbors(2), 10), "Tb does not see Ta's edge (snapshot isolation)");
  assert(tb->commit());
  std::cout << "Tb committed (edge 2→20)" << std::endl;

  // A fresh transaction sees both edges.
  auto t_fresh = db.begin_txn();
  check(has_neighbor(t_fresh->get_neighbors(2), 10), "fresh txn sees 2→10");
  check(has_neighbor(t_fresh->get_neighbors(2), 20), "fresh txn sees 2→20");
  t_fresh->abort();

  std::cout << "=== Concurrent writer test complete ===" << std::endl;
}

// ─── Test 4: CSR per-edge timestamp correctness after SMO ────────────────────

static void run_csr_edge_ts_test(bw_graph_db_t& db) {
  std::cout << "\n=== CSR Per-Edge Timestamp Test ===" << std::endl;

  // Force enough inserts on vertex 3 to trigger SMO consolidation.
  // Read the result before and after consolidation to confirm visibility.
  constexpr int kEdges = 30;

  // Pre-consolidation: insert many edges from vertex 3.
  std::vector<v_id_t> inserted;
  for (int i = 0; i < kEdges; ++i) {
    v_id_t dst = static_cast<v_id_t>(100 + i);
    auto ti = db.begin_txn();
    ti->insert_edge(3, dst);
    if (ti->commit())
      inserted.push_back(dst);
  }
  std::cout << "Inserted " << inserted.size() << " edges from vertex 3" << std::endl;

  // Wait for any async consolidation to settle.
  db.smo_ctl->wait_all_consolidations();

  // Snapshot taken after all inserts: must see all inserted edges.
  auto t_read = db.begin_txn();
  auto nbrs = t_read->get_neighbors(3);
  int found = 0;
  for (v_id_t dst : inserted) {
    if (has_neighbor(nbrs, dst))
      ++found;
  }
  std::cout << "Post-consolidation: found " << found << "/" << inserted.size()
            << " edges from vertex 3" << std::endl;
  check(found == static_cast<int>(inserted.size()),
        "all inserted edges visible after SMO consolidation");
  t_read->abort();

  std::cout << "=== CSR per-edge timestamp test complete ===" << std::endl;
}

#endif // BW_GRAPH_ENABLE_TRANSACTION

int main(int argc, char* argv[]) {
  std::string config_path = "config/default.yaml";

  if (argc > 1) {
    config_path = argv[1];
    load_yaml_config(config_path);
  } else {
    load_yaml_config();
  }

  std::string g_name = bw_graph::GRAPH_NAME;

  std::cout << "Creating database..." << std::endl;
  auto db =
      bw_graph_db_t(g_name, bw_graph::BW_GRAPH_REBUILD_INDEX, bw_graph::BW_GRAPH_REDO_PARTITION);
  std::cout << "Database created" << std::endl;

#ifdef BW_GRAPH_ENABLE_TRANSACTION
  run_smoke_test(db);
  run_delete_test(db);
  run_concurrent_writer_test(db);
  run_csr_edge_ts_test(db);
#else
  std::cout << "Transaction support not compiled in (rebuild with --txn to enable)." << std::endl;
#endif

  return 0;
}
