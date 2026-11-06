#include "bw_graph/db/db.h"
#include "bw_graph/part/louvain.h"
#include "bw_graph/mem/mem_g0com.h"
#include <tbb/global_control.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <numeric>
#include <thread>
#include <unordered_set>

using namespace bw_graph;
namespace {
using bench_clock_t = std::chrono::steady_clock;
double seconds(bench_clock_t::time_point a, bench_clock_t::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}
struct quiet_t : std::streambuf {
  std::streambuf* old = std::cout.rdbuf(this);
  int overflow(int c) override { return traits_type::not_eof(c); }
  ~quiet_t() { if (old) std::cout.rdbuf(old); }
  void restore() { if (old) std::cout.rdbuf(std::exchange(old, nullptr)); }
};
void validate_tat(bw_graph_db_t& db) {
  if (db.multiscale) { db.multiscale->tat().validate(); return; }
  if (!BW_GRAPH_LEVELED) return;
  if (!db.root_index_page) throw std::runtime_error("Missing legacy TAT root");
  std::unordered_set<page_no_t> seen, leaves;
  std::function<void(index_page_t*, page_no_t)> visit = [&](auto* page, auto parent) {
    if (!seen.insert(page->get_page_no()).second || page->parent_page_no_ != parent)
      throw std::runtime_error("Invalid legacy TAT parent/ownership");
    for (auto child : page->get_blocks()) {
      auto it = db.index_page_map.find(child);
      if (it != db.index_page_map.end()) visit(it->second, page->get_page_no());
      else if (!leaves.insert(child).second)
        throw std::runtime_error("Duplicate legacy TAT leaf");
    }
  };
  visit(db.root_index_page, 0);
  if (seen.size() != db.index_page_map.size()) throw std::runtime_error("Unreachable legacy TAT");
  for (v_id_t v = 0; v < db.get_vertex_count(); ++v)
    if (!leaves.contains(db.vertex_index->get_vertex_location(v).first))
      throw std::runtime_error("Missing legacy TAT leaf");
}
void drain(bw_graph_db_t& db) {
  db.consolidate_all_pages();
}
}
int main(int argc, char** argv) {
  try {
    if (argc < 5) throw std::invalid_argument(
        "Usage: repo dataset legacy|multiscale new-store-dir [MiB=128] [writers=8] "
        "[SMO=1] [delta_pages=2] [delta_fraction=0.5] [chunks=1] [layout=kaminpar-gorder|louvain-gorder|id-based]");
    std::filesystem::current_path(argv[1]);
    const std::string name = argv[2], engine = argv[3];
    const std::filesystem::path prefix = argv[4];
    const double mib = argc > 5 ? std::stod(argv[5]) : 128;
    const size_t threads = argc > 6 ? std::stoul(argv[6]) : 8;
    const size_t smo = argc > 7 ? std::stoul(argv[7]) : 1;
    if (!(mib > 0) || !threads || !smo || (engine != "legacy" && engine != "multiscale"))
      throw std::invalid_argument("Invalid budget/threads/engine");
    const bool verify = (argc > 8 && std::string(argv[8]) == "verify") ||
                        (argc > 9 && std::string(argv[9]) == "verify");
    const size_t delta_pages = argc > 8 && std::string(argv[8]) != "verify" ?
        std::stoul(argv[8]) : 2;
    const double delta_fraction = argc > 9 && std::string(argv[9]) != "verify" ?
        std::stod(argv[9]) : 0.5;
    if (!(delta_fraction > 0 && delta_fraction < 1))
      throw std::invalid_argument("Delta fraction must be between zero and one");
    const size_t chunks = argc > 10 ? std::stoul(argv[10]) : 1;
    const auto layout_kind = parse_graph_layout(argc > 11 ? argv[11] : "kaminpar-gorder");
    const std::string layout = graph_layout_name(layout_kind);
    const bool id_order = layout_kind == graph_layout_t::id_based;
    if (!chunks) throw std::invalid_argument("Chunk count must be positive");
    if (!delta_pages) throw std::invalid_argument("Delta page threshold must be positive");
    if (!verify && std::filesystem::exists(prefix))
      throw std::invalid_argument("Use a fresh store directory");
    std::filesystem::create_directories(prefix);
    tbb::global_control limit(tbb::global_control::max_allowed_parallelism, threads + smo);
    quiet_t quiet;
    load_yaml_config("config/" + name.substr(0, 2) + ".yaml");
    GRAPH_NAME = name; CSR_PREFIX = DELTA_PREFIX = META_PREFIX = prefix.string();
    BW_MULTISCALE_ENGINE = engine == "multiscale";
    BW_GRAPH_LAYOUT = layout_kind;
    BW_GRAPH_ID_AWARE = id_order;
    BW_GRAPH_LEVELED = !id_order;
    BW_GRAPH_REDO_PARTITION = !verify;
    BW_GRAPH_CONSOLIDATION_WORKER_COUNT = smo;
    BW_GRAPH_MAX_DELTA_CHAIN_LENGTH = delta_pages;
    BW_DELTA_FLUSH_INTERVAL_MS = BW_CSR_FLUSH_INTERVAL_MS = 0;
    BW_MULTISCALE_FULL_GIANT_WARMUP = false;
    // Retain the legacy dataset page configuration. Total allocated frame
    // bytes are reported; index, SMO scratch and RocksDB are additional.
    // Keep the dataset configuration's original legacy CSR page size.
    BW_BUFFER_CHUNK_COUNT = BW_DELTA_BUFFER_CHUNK_COUNT = 1;
    const uint64_t bytes = mib * 1048576;
    const uint64_t requested_delta = bytes * delta_fraction;
    BW_BUFFER_CHUNK_SIZE = (bytes - requested_delta) / BW_GRAPH_PAGE_SIZE;
    BW_DELTA_BUFFER_CHUNK_SIZE = requested_delta / 4096;
    uint64_t actual_budget = 0;
    std::vector<std::vector<v_id_t>> expected;
    { mem_sub_com_t graph("data/" + name + ".graph", true, LOAD_BUFFER_SIZE, true, BW_MULTISCALE_ENGINE);
      expected.resize(graph.vertex_count());
      for (v_id_t v = 0; v < expected.size(); ++v) {
        auto edges = graph.get_neighbors_span(v); expected[v].assign(edges.begin(), edges.end());
      }
    }
    std::vector<std::pair<v_id_t,v_id_t>> updates;
    { std::ifstream input("data/" + name + ".delta");
      if (!input) throw std::runtime_error("Missing delta input");
      uint32_t src, dst;
      while (input >> src >> dst) {
        if (src >= expected.size() || dst >= expected.size())
          throw std::runtime_error("Delta bounds");
        updates.emplace_back(src, dst); expected[src].push_back(dst);
      }
      if (!input.eof() || updates.empty()) throw std::runtime_error("Malformed delta input");
    }
    for (auto& edges : expected) {
      std::sort(edges.begin(), edges.end());
      edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    }
    auto& groups = BW_MULTISCALE_BUFFER_CONFIG.groups;
    for (auto& g : groups) g = {0, 0};
    size_t max_degree = 0;
    for (const auto& edges : expected) max_degree = std::max(max_degree, edges.size());
    uint64_t large_bytes = 0;
    const size_t sizes[]{4096,32768,262144,2097152,16384,4096};
    for (size_t c = 1; c < 4; ++c) if (max_degree >= (sizes[c] - 48) / 4) {
      size_t count = std::max<uint64_t>(2 * smo, bytes / 32 / sizes[c]);
      groups[c] = {1, count}; large_bytes += count * sizes[c];
    }
    // Each writer holds <=1 Delta frame; each SMO worker holds <=1 Delta
    // and <=2 CSR frames. Rebalance tiny budgets instead of requiring 50:50.
    const uint64_t minimum_delta = (threads + smo) * 4096;
    const uint64_t minimum_csr = 2 * smo * 4096;
    if (large_bytes + minimum_delta + minimum_csr > bytes)
      throw std::invalid_argument("Budget below concurrent frame working set");
    const uint64_t delta_bytes = std::clamp<uint64_t>(requested_delta / 4096 * 4096,
        minimum_delta, (bytes - large_bytes - minimum_csr) / 4096 * 4096);
    groups[0] = {1, (bytes - large_bytes - delta_bytes) / 4096};
    groups[5] = {1, delta_bytes / 4096};
    // Change geometry without rounding away frames or expanding their budget.
    for (size_t c = 0; c < 6; ++c) if (groups[c].chunk_size) {
      groups[c].chunk_count = std::gcd(chunks, groups[c].chunk_size);
      groups[c].chunk_size /= groups[c].chunk_count;
      actual_budget += groups[c].chunk_count * groups[c].chunk_size * sizes[c];
    }
    if (!BW_MULTISCALE_ENGINE) {
      if (!BW_BUFFER_CHUNK_SIZE || !BW_DELTA_BUFFER_CHUNK_SIZE)
        throw std::invalid_argument("Budget below legacy chunk size");
      BW_BUFFER_CHUNK_COUNT = std::gcd(chunks, BW_BUFFER_CHUNK_SIZE);
      BW_DELTA_BUFFER_CHUNK_COUNT = std::gcd(chunks, BW_DELTA_BUFFER_CHUNK_SIZE);
      BW_BUFFER_CHUNK_SIZE /= BW_BUFFER_CHUNK_COUNT;
      BW_DELTA_BUFFER_CHUNK_SIZE /= BW_DELTA_BUFFER_CHUNK_COUNT;
      actual_budget = BW_BUFFER_CHUNK_COUNT * BW_BUFFER_CHUNK_SIZE * BW_GRAPH_PAGE_SIZE +
          BW_DELTA_BUFFER_CHUNK_COUNT * BW_DELTA_BUFFER_CHUNK_SIZE * 4096;
    }
    std::mt19937 rng(42); std::shuffle(updates.begin(), updates.end(), rng);
    last_louvain_build = {};
    const auto build_start = bench_clock_t::now();
    auto db = std::make_unique<bw_graph_db_t>(name, !verify, !verify, false, id_order, true, !id_order);
    const auto build_end = bench_clock_t::now();
    const auto layout_stats = last_louvain_build;
    validate_tat(*db);
    db->warm_up();
    std::atomic<bool> failed{false};
    auto io_totals = [&] {
      std::array<uint64_t, 4> total{};
      if (db->multiscale) for (unsigned c = 0; c < 6; ++c) {
        auto stats = db->multiscale->pool().stats(static_cast<page_space_t>(c));
        total[0] += stats.disk_reads; total[1] += stats.writebacks; total[2] += stats.evictions;
        if (c == 5) total[3] = stats.disk_reads;
      }
      return total;
    };
    const auto initial_io = io_totals();
    const auto start = bench_clock_t::now();
    std::vector<std::thread> writers;
    for (size_t t = 0; t < (verify ? 0 : threads); ++t) writers.emplace_back([&, t] {
      try {
        for (size_t i = t; i < updates.size(); i += threads)
          if (!db->insert_edge(updates[i].first, updates[i].second)) failed = true;
      } catch (...) { failed = true; }
    });
    for (auto& w : writers) w.join();
    const auto appended = bench_clock_t::now();
    if (failed) throw std::runtime_error("Writer failure");
    if (!verify) drain(*db);
    const auto drained = bench_clock_t::now();
    const auto stats = db->multiscale ? db->multiscale->smo_stats() :
        multiscale_store_t::smo_stats_t{};
    const auto final_io = io_totals();
    multiscale_tat_t::stats_t tat{};
    if (db->multiscale) tat = db->multiscale->tat().stats();
    if (!id_order && !db->multiscale && !db->root_index_page)
      throw std::runtime_error("Legacy TAT not built");
    db->close(); db.reset();
    const auto closed = bench_clock_t::now();
    db = std::make_unique<bw_graph_db_t>(name, false, false, false, id_order, true, !id_order);
    validate_tat(*db);
    for (v_id_t v = 0; v < expected.size(); ++v) {
      auto values = db->read_neighbor_clone(v);
      std::sort(values.begin(), values.end());
      if (values != expected[v]) {
        std::vector<v_id_t> missing, extra;
        std::set_difference(expected[v].begin(), expected[v].end(), values.begin(), values.end(),
                            std::back_inserter(missing));
        std::set_difference(values.begin(), values.end(), expected[v].begin(), expected[v].end(),
                            std::back_inserter(extra));
        throw std::runtime_error("Reopened adjacency mismatch at " + std::to_string(v) +
            " actual=" + std::to_string(values.size()) +
            " expected=" + std::to_string(expected[v].size()) +
            " missing=" + (missing.empty() ? "none" : std::to_string(missing.front())) +
            " extra=" + (extra.empty() ? "none" : std::to_string(extra.front())));
      }
    }
    db->close(); db.reset();
    quiet.restore();
    if (verify) {
      std::cout << "[pass] verify-only " << engine << ' ' << name << '\n';
      return 0;
    }
    std::cout << std::fixed << std::setprecision(6) << "[pass] " << engine << ' ' << name
              << " layout=" << layout << " MiB=" << actual_budget/1048576.0
              << " legacy_page_KiB=" << BW_GRAPH_PAGE_SIZE/1024 << " writers=" << threads
              << " SMO=" << smo << " chunks=" << chunks << " delta_pages=" << delta_pages
              << " delta_fraction=" << delta_fraction << " edges=" << updates.size()
              << " build_s=" << seconds(build_start,build_end)
              << " louvain_threads=" << layout_stats.threads
              << " communities=" << layout_stats.communities
              << " foreground_Medges_s=" << updates.size()/seconds(start,appended)/1e6
              << " append_s=" << seconds(start,appended)
              << " drain_s=" << seconds(appended,drained) << " close_s=" << seconds(drained,closed)
              << " total_s=" << seconds(start,closed)
              << " Medges_s=" << updates.size()/seconds(start,closed)/1e6;
    if (BW_MULTISCALE_ENGINE)
      std::cout << " jobs=" << stats.jobs << " background=" << stats.background_jobs
                << " merged=" << stats.records
                << " SMO_MiB=" << stats.bytes_written/1048576.0
                << " reads=" << final_io[0] - initial_io[0]
                << " writebacks=" << final_io[1] - initial_io[1]
                << " evictions=" << final_io[2] - initial_io[2]
                << " delta_reads=" << final_io[3] - initial_io[3]
                << " tat_height=" << tat.height << " tat_leaves=" << tat.leaves
                << " tat_replace=" << tat.replacements << " tat_split=" << tat.splits;
    std::cout << '\n';
    return 0;
  } catch (const std::exception& e) { std::cerr << "[fail] " << e.what() << '\n'; }
  catch (...) { std::cerr << "[fail] unknown benchmark exception\n"; }
  return 1;
}
