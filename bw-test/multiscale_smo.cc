#include "vertex_backend_option.h"
#include "bw_graph/db/multiscale_store.h"
#include "bw_graph/common/config.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>
#include <unistd.h>

using namespace bw_graph;
namespace {
struct quiet_import_t : std::streambuf {
  std::streambuf* old = std::cout.rdbuf(this);
  int overflow(int c) override { return traits_type::not_eof(c); }
  ~quiet_import_t() { std::cout.rdbuf(old); }
};
std::vector<v_id_t> ordered(std::vector<v_id_t> v) {
  std::sort(v.begin(), v.end()); return v;
}
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
}
int main(int argc, char** argv) {
  [[maybe_unused]] const auto vertex_backend = vertex_backend_option(argc, argv);
  const auto layout = argc > 2 && std::string(argv[1]) == "--layout" ?
      parse_graph_layout(argv[2]) : (argc > 1 && std::string(argv[1]) == "--partition" ?
      graph_layout_t::kaminpar_gorder : graph_layout_t::id_based);
  BW_GRAPH_LAYOUT = layout;
  const bool id_order = layout == graph_layout_t::id_based;
  quiet_import_t quiet;
  try {
    char pattern[] = "/tmp/bw-smo-mtr-XXXXXX";
    auto* temp = mkdtemp(pattern);
    if (!temp) throw std::runtime_error("temporary directory");
    const std::filesystem::path root(temp), file = root / "graph";
    { std::ofstream out(file); out << "t 30000 24000\n";
      for (unsigned v = 1; v <= 12000; ++v) out << "e 0 " << v << "\ne " << v << " 0\n"; }
    multiscale_buf_options_t budget;
    for (auto& g : budget.groups) g = {2, 8};
    std::vector<v_id_t> expected;
    {
      multiscale_store_t db(root / "db", true, id_order, file.string(), budget, 2, 1);
      auto old = db.read_neighbor(0);
      require(old.size() == 12000, "initial giant");
      db.update(0, 1, DELETE); db.update(0, 20000, INSERT);
      db.consolidate_all();
      db.tat().validate();
      auto values = ordered(db.clone(0));
      require(values.size() == 12000 && values.front() == 2 && values.back() == 20000,
              "forced consolidation applies insert/delete");
      size_t old_count = 0;
      for (auto v : old) { require(v >= 1 && v <= 12000, "old view changed"); ++old_count; }
      require(old_count == 12000, "old view lifetime across publication");
      std::cerr << "ok - giant consolidation and borrowed old version\n";
      std::atomic<bool> bad{false}, done{false};
      std::vector<std::thread> writers;
      for (unsigned t = 0; t < 4; ++t) writers.emplace_back([&, t] {
        try {
          for (unsigned i = 0; i < 500; ++i) {
            auto dst = 21000 + t * 500 + i;
            require(db.update(0, dst, INSERT), "insert");
            require(db.update(0, dst, DELETE), "delete");
            require(db.update(0, dst, INSERT), "reinsert");
            require(db.update(1 + t * 5000, dst, INSERT), "small insert");
          }
        } catch (...) { bad = true; }
      });
      std::thread reader([&] {
        try {
          while (!done) {
            auto values = ordered(db.clone(0));
            require(std::adjacent_find(values.begin(), values.end()) == values.end(), "duplicate");
            require(std::binary_search(values.begin(), values.end(), 20000), "lost committed edge");
            auto snapshot = db.read_neighbor(0);
            size_t count = 0;
            for (auto v : snapshot) { require(v < 30000, "invalid neighbor"); ++count; }
            require(count == snapshot.size(), "range size");
          }
        } catch (...) { bad = true; }
      });
      for (auto& w : writers) w.join();
      done = true;
      reader.join();
      db.wait_for_smo();
      require(!bad, "concurrent updates/read/SMO");
      db.consolidate_all();
      db.tat().validate();
      expected = ordered(db.clone(0));
      require(expected.size() == 14000, "final giant count");
      for (unsigned v = 21000; v < 23000; ++v)
        require(std::binary_search(expected.begin(), expected.end(), v), "missing update");
      for (unsigned t = 0; t < 4; ++t)
        require(db.clone(1 + t * 5000).size() == (t < 3 ? 501 : 500), "small final count");
      auto base = db.read_neighbor(0);
      require(base.delta_head() == base.delta_stop(), "force leaves pending records");
      require(db.smo_stats().jobs > 2, "background SMO did not execute");
      std::cerr << "ok - concurrent read/write and active background SMO\n";
    }
    for (auto& g : budget.groups) g = {1, 3};
    {
      multiscale_store_t db(root / "db", false, id_order, "", budget, 1, 1);
      require(ordered(db.clone(0)) == expected, "reopen");
      for (unsigned v = 2; v <= 12000; ++v) db.update(0, v, DELETE);
      db.consolidate_all();
      db.tat().validate();
      auto values = ordered(db.clone(0));
      require(values.size() == 2001 && values.front() == 20000, "delete/downsize");
      std::cerr << "ok - reopen, tiny cache and giant shrink\n";
    }
#ifdef BW_GRAPH_ENABLE_TRANSACTION
    {
      multiscale_store_t db(root / "db", false, id_order, "", budget, 1, 1);
      auto old = db.begin_snapshot();
      const auto before = ordered(db.clone_at(0, old));
      std::vector<write_opt_t> writes{{write_ops_type_t::INSERT_EDGE, 0, 29000},
                                     {write_ops_type_t::DELETE_EDGE, 0, 20000},
                                     {write_ops_type_t::INSERT_EDGE, 4, 29001}};
      require(db.commit_edges(old, writes), "transaction commit");
      auto committed_record = db.read_delta(db.delta_head(0));
      require(committed_record.begin_ts == old && committed_record.commit_ts > old,
              "transaction record timestamps");
      auto fresh = db.begin_snapshot();
      require(ordered(db.clone_at(0, old)) == before, "old snapshot after commit");
      auto after = ordered(db.clone_at(0, fresh));
      require(std::binary_search(after.begin(), after.end(), 29000) &&
              !std::binary_search(after.begin(), after.end(), 20000), "new snapshot");
      db.consolidate_all();
      db.tat().validate();
      require(ordered(db.clone_at(0, old)) == before, "old snapshot across SMO");
      require(ordered(db.clone_at(0, fresh)) == after, "new snapshot across SMO");
      auto base = db.read_neighbor(0);
      for (auto dst : base) require(dst != 29000, "watermark violation");
      require(!db.commit_edges(old, writes), "first-committer-wins conflict");
      std::vector<write_opt_t> invalid{{write_ops_type_t::INSERT_EDGE, 0, 29002},
                                      {write_ops_type_t::INSERT_EDGE, 0, 50000}};
      require(!db.commit_edges(fresh, invalid), "invalid transaction rejected");
      require(ordered(db.clone_at(0, fresh)) == after, "failed commit leaked data");
      db.end_snapshot(old); db.end_snapshot(fresh);
      db.consolidate_all();
      db.tat().validate();
      require(ordered(db.clone(0)) == after, "post-watermark full merge");
      std::cerr << "ok - snapshot isolation, watermark, conflict and atomic validation\n";
    }
#endif
    std::filesystem::remove_all(root);
    std::cerr << "PASS multiscale_smo layout=" << graph_layout_name(layout) << '\n';
    return 0;
  } catch (const std::exception& e) { std::cerr << "FAIL multiscale_smo: " << e.what() << '\n'; }
  catch (...) { std::cerr << "FAIL multiscale_smo: unknown exception\n"; }
  return 1;
}
