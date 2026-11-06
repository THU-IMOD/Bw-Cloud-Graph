#include "vertex_backend_option.h"
#include "bw_graph/db/db.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace bw_graph;
namespace {
struct quiet_import_t : std::streambuf {
  std::streambuf* old = std::cout.rdbuf(this);
  int overflow(int c) override { return traits_type::not_eof(c); }
  ~quiet_import_t() { std::cout.rdbuf(old); }
};
#ifdef BW_GRAPH_ENABLE_TRANSACTION
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
bool has(const std::vector<v_id_t>& v, v_id_t x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}
#endif
}
int main(int argc, char** argv) {
  [[maybe_unused]] const auto vertex_backend = vertex_backend_option(argc, argv);
#ifndef BW_GRAPH_ENABLE_TRANSACTION
  std::cerr << "SKIP multiscale_txn: build with --txn\n";
  return 0;
#else
  quiet_import_t quiet;
  try {
    const auto repo = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path();
    load_yaml_config((repo / "config/example.yaml").string());
    BW_VERTEX_INDEX_BACKEND = vertex_backend;
    const auto layout = argc > 3 && std::string(argv[2]) == "--layout" ?
        parse_graph_layout(argv[3]) : (argc > 2 && std::string(argv[2]) == "--partition" ?
        graph_layout_t::kaminpar_gorder : graph_layout_t::id_based);
    BW_GRAPH_LAYOUT = layout;
    const bool id_order = layout == graph_layout_t::id_based;
    char pattern[] = "/tmp/bw-smo-txn-XXXXXX";
    auto* temp = mkdtemp(pattern); require(temp, "temporary directory");
    const std::filesystem::path root(temp);
    std::filesystem::create_directory(root / "data");
    { std::ofstream out(root / "data/snapshot.graph"); out << "t 512 2\ne 0 1\ne 2 3\n"; }
    CSR_PREFIX = DELTA_PREFIX = META_PREFIX = (root / "store").string();
    BW_MULTISCALE_ENGINE = true; BW_GRAPH_CONSOLIDATION_WORKER_COUNT = 2;
    BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = 1;
    for (auto& g : BW_MULTISCALE_BUFFER_CONFIG.groups) g = {2, 4};
    std::filesystem::current_path(root);
    {
      bw_graph_db_t db("snapshot", true, true, false, id_order);
      auto old = db.begin_txn();
      auto writer = db.begin_txn();
      writer->insert_edge(0, 4); writer->delete_edge(0, 1); writer->insert_edge(2, 5);
      require(has(writer->get_neighbors(0), 4) && !has(writer->get_neighbors(0), 1),
              "read own writes");
      require(has(writer->get_neighbors(4, direction_t::in), 0), "incoming own writes");
      require(!has(old->get_neighbors(4, direction_t::in), 0), "incoming dirty read");
      require(!has(old->get_neighbors(0), 4), "dirty read");
      require(writer->commit(), "commit"); writer.reset();
      require(has(old->get_neighbors(0), 1) && !has(old->get_neighbors(0), 4), "snapshot changed");
      auto a = db.begin_txn(), b = db.begin_txn();
      a->insert_edge(0, 7); b->delete_edge(0, 7);
      require(a->commit() && !b->commit(), "write conflict"); a.reset(); b.reset();
      std::atomic<bool> failed{false}, done{false};
      std::jthread updater([&] {
        try {
          for (v_id_t i = 10; i < 200; ++i) {
            auto tx = db.begin_txn(); tx->insert_edge(0, i); tx->insert_edge(300, i);
            require(tx->commit(), "concurrent commit");
          }
        } catch (...) { failed = true; }
        done = true;
      });
      while (!done) {
        auto tx = db.begin_txn(); auto x = tx->get_neighbors(0), y = tx->get_neighbors(300);
        for (v_id_t i = 10; i < 200; ++i)
          require(has(x, i) == has(y, i), "partial multi-vertex commit");
        for (v_id_t i = 10; i < 200; ++i) {
          const auto incoming = tx->get_neighbors(i, direction_t::in);
          require(has(x, i) == has(incoming, 0) && has(y, i) == has(incoming, 300),
                  "OUT/IN commit visibility differs");
        }
        require(old->get_neighbors(0) == std::vector<v_id_t>{1}, "long snapshot changed");
      }
      updater.join(); require(!failed, "concurrent writer");
      db.consolidate_all_pages();
      require(old->get_neighbors(0) == std::vector<v_id_t>{1}, "snapshot after SMO");
      old.reset(); db.consolidate_all_pages();
      auto tx = db.begin_txn(); require(has(tx->get_neighbors(0), 199), "merged visibility");
      tx.reset();
      // Move the watermark between two commits: fold the older prefix while
      // retaining records that are invisible to the still-active snapshot.
      auto first = db.begin_txn();
      auto update = db.begin_txn(); update->insert_edge(4, 400);
      require(update->commit(), "first prefix commit"); update.reset();
      auto middle = db.begin_txn();
      update = db.begin_txn(); update->delete_edge(4, 400); update->insert_edge(4, 401);
      require(update->commit(), "second prefix commit"); update.reset();
      first.reset(); db.consolidate_all_pages();
      require(has(middle->get_neighbors(4), 400) && !has(middle->get_neighbors(4), 401),
              "partially advanced watermark snapshot");
      auto current = db.begin_txn();
      require(!has(current->get_neighbors(4), 400) && has(current->get_neighbors(4), 401),
              "new reader after partial consolidation");
      middle.reset(); current.reset(); db.consolidate_all_pages();
      require(db.multiscale->smo_stats().jobs > 0, "SMO was disabled");
      std::cerr << "ok - public txn API: snapshots, own writes, conflicts and atomic commits\n";
    }
    {
      bw_graph_db_t db("snapshot", false, false, false, id_order);
      auto tx = db.begin_txn();
      require(has(tx->get_neighbors(0), 199) && !has(tx->get_neighbors(0), 1), "reopen visibility");
    }
    std::filesystem::current_path(repo); std::filesystem::remove_all(root);
    std::cerr << "PASS multiscale_txn\n"; return 0;
  } catch (const std::exception& e) { std::cerr << "FAIL multiscale_txn: " << e.what() << '\n'; }
  catch (...) { std::cerr << "FAIL multiscale_txn: unknown exception\n"; }
  return 1;
#endif
}
