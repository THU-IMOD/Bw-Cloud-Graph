#include "bw_graph/buf/buf_pool.h"
#include "bw_graph/buf/multiscale_buf_pool.h"
#include "bw_graph/io/io_csr.h"
#include "bw_graph/io/multiscale_csr_page.h"
#include "bw_graph/io/multiscale_csr_page_view.h"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <unistd.h>

using namespace bw_graph;
namespace {
using bench_clock = std::chrono::steady_clock;
struct options_t {
  size_t pages{8192}, frames{512}, rounds{3}, depth{16}, hot_ops{1000000};
  size_t objects{2048}, queries{4096};
  std::string suite{"all"};
};
options_t parse(int argc, char** argv) {
  options_t o;
  int start = argc > 1 && argv[1][0] != '-' ? 2 : 1;
  for (int i = start; i < argc; ++i) {
    std::string key = argv[i];
    if (key == "--help") {
      std::cout << "multiscale_loading_bench [--suite fixed|mixed|all] [--pages N] "
          "[--frames N] [--rounds N] [--depth N] [--hot-ops N] [--objects N] [--queries N]\n";
      std::exit(0);
    }
    if (++i == argc) throw std::invalid_argument("Missing value");
    if (key == "--suite") { o.suite = argv[i]; continue; }
    size_t n = parse_buffer_count(YAML::Node(argv[i]));
    if (!n || n > 100000000) throw std::invalid_argument("Invalid benchmark size");
    if (key == "--pages") o.pages = n;
    else if (key == "--frames") o.frames = n;
    else if (key == "--rounds") o.rounds = n;
    else if (key == "--depth") o.depth = n;
    else if (key == "--hot-ops") o.hot_ops = n;
    else if (key == "--objects") o.objects = n;
    else if (key == "--queries") o.queries = n;
    else throw std::invalid_argument("Unknown option");
  }
  if (o.frames < 16 || o.frames >= o.pages || o.depth >= o.frames || o.objects < 100)
    throw std::invalid_argument("Require 16 <= frames < pages, depth < frames, objects >= 100");
  if (o.suite != "fixed" && o.suite != "mixed" && o.suite != "all")
    throw std::invalid_argument("Invalid suite");
  return o;
}
struct store_t {
  std::filesystem::path path;
  store_t() {
    char pattern[] = "/tmp/bw-loading-XXXXXX";
    if (!mkdtemp(pattern)) throw std::runtime_error("mkdtemp failed");
    path = pattern;
  }
  ~store_t() { std::error_code error; std::filesystem::remove_all(path, error); }
};
// Observe legacy I/O without changing its implementation or cache policy.
struct legacy_disk_t : disk_manager_t {
  std::atomic<uint64_t> reads{0}, writes{0};
  size_t bias{0};
  legacy_disk_t(const std::filesystem::path& p, size_t size, size_t page_bias = 0)
      : disk_manager_t(p, size), bias(page_bias) {}
  ~legacy_disk_t() override { shutdown(); }
  void read_page(page_no_t id, char* bytes) override {
    reads.fetch_add(1, std::memory_order_relaxed);
    disk_manager_t::read_page(id + bias, bytes);
  }
  void write_page(page_no_t id, const char* bytes) override {
    writes.fetch_add(1, std::memory_order_relaxed);
    disk_manager_t::write_page(id + bias, bytes);
  }
};
uint32_t neighbor(uint32_t vertex, uint32_t edge) {
  return (vertex * 2654435761u + edge * 2246822519u) & 0xffffff;
}
uint64_t consume(const std::byte* data, size_t count) {
  uint64_t sum = 0;
  for (size_t i = 0; i < count; ++i) {
    uint32_t value;
    std::memcpy(&value, data + i * 4, 4);
    sum += value;
  }
  return sum;
}
struct row_t {
  double seconds{0};
  uint64_t checksum{0}, reads{0}, read_bytes{0}, hits{0}, coalesced{0}, writes{0};
};
void print_row(const std::string& label, std::vector<row_t> samples, uint64_t expected,
               size_t ops, double reference = 0) {
  for (auto r : samples) if (r.checksum != expected) throw std::runtime_error("Checksum: " + label);
  std::sort(samples.begin(), samples.end(), [](auto a, auto b) { return a.seconds < b.seconds; });
  auto r = samples[samples.size() / 2];
  std::cout << std::left << std::setw(27) << label << " [pass] " << std::right
            << std::fixed << std::setprecision(4) << std::setw(8) << r.seconds
            << std::setprecision(2) << std::setw(10) << ops / r.seconds / 1000
            << std::setw(9) << (reference ? reference / r.seconds : 1.0)
            << std::setw(10) << r.reads << std::setw(9) << r.writes
            << std::setw(10) << r.read_bytes / 1048576.0
            << std::setw(10) << r.hits << std::setw(10) << r.coalesced
            << std::setprecision(4) << "  " << samples.front().seconds << ".."
            << samples.back().seconds << '\n';
}
double median_seconds(std::vector<row_t> samples) {
  std::sort(samples.begin(), samples.end(), [](auto a, auto b) { return a.seconds < b.seconds; });
  return samples[samples.size() / 2].seconds;
}
multiscale_buf_options_t fixed_budget(size_t frames) {
  multiscale_buf_options_t options;
  for (auto& g : options.groups) g = {0, 0};
  // Same eight chunks and same frame bytes on both implementations.
  options.groups[0] = {8, (frames + 7) / 8};
  return options;
}
void run_fixed(const options_t& o) {
  BW_GRAPH_PAGE_SIZE = 4096;
  BW_GRAPH_MAX_BLOCK_NEIGHBOR = 4;
  BW_GRAPH_GIANT_DEGREE_BOUND = UINT32_MAX;
  BW_BUFFER_PREFETCH_WORKER_COUNT = 4;
  const size_t frames = ((o.frames + 7) / 8) * 8;
  const size_t payload = 16 + 4 * sizeof(page_no_t) + sizeof(csr_vertex_t);
  constexpr uint16_t degree = 768;
  store_t store;
  multiscale_disk_options_t disk_options;
#ifdef __linux__
  // Legacy Linux uses buffered pread, not O_DIRECT. Match it for this comparison.
  disk_options.bypass_cache = false;
#endif
  multiscale_disk_manager_t disk(store.path / "multi", disk_options);
  std::vector<uint64_t> sums(o.pages);
  disk_page_t image(page_id_t::make(page_space_t::CSR_4K, 0), 4096);
  for (size_t v = 0; v < o.pages; ++v) {
    auto bytes = image.bytes();
    std::fill(bytes.begin(), bytes.end(), std::byte{0});
    const uint16_t vertices = 1;
    const uint32_t used = payload + degree * 4;
    std::memcpy(bytes.data(), &vertices, 2);
    std::memcpy(bytes.data() + 2, &degree, 2);
    std::memcpy(bytes.data() + 4, &used, 4);
    csr_vertex_t record(v, degree, 0);
    std::memcpy(bytes.data() + payload - sizeof(record), &record, sizeof(record));
    for (uint32_t e = 0; e < degree; ++e) {
      const auto value = neighbor(v, e);
      std::memcpy(bytes.data() + payload + e * 4, &value, 4);
      sums[v] += value;
    }
    disk.write_page(disk.allocate_page(page_space_t::CSR_4K), bytes);
  }
  disk.flush();
  // Both unchanged disk implementations read the SAME physical payload. Legacy
  // page numbers are shifted past the 4 KiB file header. Accesses/updates are
  // serialized between engines; the legacy manager does not take a flock.
  const auto legacy_file = store.path / "multi" / "csr_4k.pages";
  legacy_disk_t legacy(legacy_file, 4096, 1);
  std::vector<std::pair<std::string, std::vector<uint32_t>>> traces;
  std::vector<uint32_t> cold(o.pages);
  std::iota(cold.begin(), cold.end(), 0);
  traces.push_back({"scan", cold});
  std::mt19937 rng(20261029);
  std::vector<uint32_t> local(o.pages * 2);
  for (auto& v : local)
    v = rng() % 100 < 80 ? rng() % (frames / 2) : frames / 2 + rng() % (o.pages - frames / 2);
  traces.push_back({"local80", local});
  std::vector<uint32_t> frontier;
  for (size_t start = 0; start < o.pages; start += 64)
    for (size_t iteration = 0; iteration < 4; ++iteration)
      for (size_t j = start; j < std::min(start + 64, o.pages); ++j) frontier.push_back(j);
  traces.push_back({"frontier", frontier});
  std::cout << "fixed: SAME file/valid 4KiB CSR images, " << frames * 4096 / 1048576.0
            << " MiB frames each, scan ALL 768 neighbors/access; cold application caches\n";
  for (auto& [name, trace] : traces) {
    uint64_t expected = 0;
    for (auto v : trace) expected += sums[v];
    const std::array<std::string, 6> modes{
        "old_disk", "old_pool", "old_prefetch", "new_disk", "new_pool", "new_batch"};
    std::array<std::vector<row_t>, 6> results;
    for (size_t round = 0; round < o.rounds; ++round) {
      // Rotate execution order between repetitions; do not always favor the second store.
      for (size_t k = 0; k < modes.size(); ++k) {
        const size_t mode = (k + round) % modes.size();
        std::unique_ptr<buf_pool_t<csr_page_t>> old_pool;
        std::unique_ptr<multiscale_buf_pool_t> pool;
        if (mode == 1 || mode == 2)
          old_pool.reset(buf_pool_t<csr_page_t>::buf_pool_init(8, frames / 8, 4096));
        if (mode >= 4) pool = std::make_unique<multiscale_buf_pool_t>(disk, fixed_budget(frames));
        disk_page_t scratch(image.page_id(), 4096);
        row_t r;
        const auto before = legacy.reads.load();
        const auto start = bench_clock::now();
        if (mode == 5) {
          for (size_t i = 0; i < trace.size(); i += o.depth) {
            std::vector<page_id_t> ids;
            for (size_t j = i; j < std::min(trace.size(), i + o.depth); ++j)
              ids.push_back(page_id_t::make(page_space_t::CSR_4K, trace[j]));
            auto pending = pool->fetch_batch(ids);
            for (auto& request : pending) {
              auto guard = request.result.get().read();
              r.checksum += consume(guard.bytes().data() + payload, degree);
            }
          }
        } else {
          if (mode == 2)
            for (size_t j = 0; j < std::min(o.depth, trace.size()); ++j)
              old_pool->buf_page_prefetch(trace[j], &legacy);
          for (size_t i = 0; i < trace.size(); ++i) {
            const auto v = trace[i];
            if (mode == 0) {
              legacy.read_page(v, reinterpret_cast<char*>(scratch.bytes().data()));
              r.checksum += consume(scratch.bytes().data() + payload, degree);
            } else if (mode == 1 || mode == 2) {
              auto* page = old_pool->buf_page_read(v, &legacy);
              r.checksum += consume(reinterpret_cast<const std::byte*>(page->get_data()) + payload,
                                    degree);
              page->r_unlatch();
              if (mode == 2 && i + o.depth < trace.size())
                old_pool->buf_page_prefetch(trace[i + o.depth], &legacy);
            } else if (mode == 3) {
              disk.read_page(page_id_t::make(page_space_t::CSR_4K, v), scratch.bytes());
              r.checksum += consume(scratch.bytes().data() + payload, degree);
            } else {
              auto guard = pool->fetch_read(page_id_t::make(page_space_t::CSR_4K, v));
              r.checksum += consume(guard.bytes().data() + payload, degree);
            }
          }
        }
        if (old_pool) old_pool->wait_for_prefetch_idle();
        r.seconds = std::chrono::duration<double>(bench_clock::now() - start).count();
        if (mode <= 2) r.reads = legacy.reads.load() - before;
        else if (mode == 3) r.reads = trace.size();
        else {
          auto stats = pool->stats(page_space_t::CSR_4K);
          r.reads = stats.disk_reads; r.hits = stats.hits; r.coalesced = stats.coalesced;
        }
        if (old_pool) r.hits = old_pool->get_stats().cache_hits;
        r.read_bytes = r.reads * 4096;
        results[mode].push_back(r);
      }
    }
    for (size_t mode = 0; mode < modes.size(); ++mode)
      print_row(name + "/" + modes[mode], results[mode], expected, trace.size(),
                median_seconds(results[0]));
  }
  // Prewarmed metadata benchmark: same access sequence, same frame/chunk budgets.
  for (size_t threads : {size_t{1}, size_t{4}}) {
    for (bool modern : {false, true}) {
      std::vector<row_t> results;
      uint64_t expected = 0;
      for (size_t j = 0; j < o.hot_ops; ++j) expected += neighbor(j % frames, 0);
      for (size_t round = 0; round < o.rounds; ++round) {
        std::unique_ptr<buf_pool_t<csr_page_t>> old_pool;
        std::unique_ptr<multiscale_buf_pool_t> pool;
        if (modern) pool = std::make_unique<multiscale_buf_pool_t>(disk, fixed_budget(frames));
        else old_pool.reset(buf_pool_t<csr_page_t>::buf_pool_init(8, frames / 8, 4096));
        for (size_t j = 0; j < frames; ++j) {
          if (modern) (void)pool->fetch_read(page_id_t::make(page_space_t::CSR_4K, j));
          else old_pool->buf_page_read(j, &legacy)->r_unlatch();
        }
        std::barrier gate(static_cast<ptrdiff_t>(threads + 1));
        std::vector<std::thread> workers;
        std::vector<uint64_t> checksums(threads);
        for (size_t t = 0; t < threads; ++t) workers.emplace_back([&, t] {
          gate.arrive_and_wait();
          uint64_t sum = 0;
          for (size_t j = t; j < o.hot_ops; j += threads) {
            const auto v = j % frames;
            if (modern) {
              auto guard = pool->fetch_read(page_id_t::make(page_space_t::CSR_4K, v));
              sum += consume(guard.bytes().data() + payload, 1);
            } else {
              auto* page = old_pool->buf_page_read(v, &legacy);
              sum += consume(reinterpret_cast<const std::byte*>(page->get_data()) + payload, 1);
              page->r_unlatch();
            }
          }
          checksums[t] = sum;
        });
        const auto start = bench_clock::now();
        gate.arrive_and_wait();
        for (auto& worker : workers) worker.join();
        row_t r;
        r.seconds = std::chrono::duration<double>(bench_clock::now() - start).count();
        r.checksum = std::accumulate(checksums.begin(), checksums.end(), uint64_t{0});
        r.hits = o.hot_ops;
        results.push_back(r);
      }
      print_row("hot_t" + std::to_string(threads) + (modern ? "/new" : "/old"), results,
                expected, o.hot_ops);
    }
  }
  // Localized updates use the same trace and cumulative values on the shared file.
  // Verification reads are outside timing; writeback and durable sync are inside.
  std::array<std::vector<row_t>, 2> updates;
  std::vector<uint32_t> changes(o.pages);
  for (auto id : local) ++changes[id];
  auto sync_legacy = [&] {
    const int fd = ::open(legacy_file.c_str(), O_RDWR);
    if (fd < 0) throw std::runtime_error("Cannot open sync fd");
#ifdef __APPLE__
    const int result = ::fcntl(fd, F_FULLFSYNC);
#else
    const int result = ::fdatasync(fd);
#endif
    ::close(fd);
    if (result != 0) throw std::runtime_error("Legacy sync failed");
  };
  for (size_t round = 0; round < o.rounds; ++round) {
    for (size_t k = 0; k < 2; ++k) {
      const size_t modern = (k + round) % 2;
      std::unique_ptr<buf_pool_t<csr_page_t>> old_pool;
      std::unique_ptr<multiscale_buf_pool_t> pool;
      if (modern) pool = std::make_unique<multiscale_buf_pool_t>(disk, fixed_budget(frames));
      else old_pool.reset(buf_pool_t<csr_page_t>::buf_pool_init(8, frames / 8, 4096));
      const auto before_read = legacy.reads.load(), before_write = legacy.writes.load();
      row_t r;
      const auto start = bench_clock::now();
      for (auto v : local) {
        auto modify = [&](std::byte* data) {
          uint32_t value;
          std::memcpy(&value, data + payload, 4);
          ++value;
          std::memcpy(data + payload, &value, 4);
        };
        if (modern) {
          auto guard = pool->fetch_write(page_id_t::make(page_space_t::CSR_4K, v));
          modify(guard.bytes().data());
        } else {
          auto* page = old_pool->buf_page_read_for_slot_write(v, &legacy);
          modify(reinterpret_cast<std::byte*>(page->get_data()));
          page->mark_dirty();
          page->w_unlatch();
        }
      }
      if (modern) pool->flush_all();
      else {
        old_pool->traverse_page_map([&](page_no_t id, csr_page_t* page) {
          page->w_latch();
          if (page->is_dirty()) { legacy.write_page(id, page->get_data()); page->mark_clean(); }
          page->w_unlatch();
          return true;
        });
        sync_legacy();
      }
      r.seconds = std::chrono::duration<double>(bench_clock::now() - start).count();
      if (modern) {
        const auto stats = pool->stats(page_space_t::CSR_4K);
        r.reads = stats.disk_reads; r.writes = stats.writebacks; r.hits = stats.hits;
      } else {
        r.reads = legacy.reads.load() - before_read;
        r.writes = legacy.writes.load() - before_write;
        r.hits = old_pool->get_stats().cache_hits;
      }
      r.read_bytes = r.reads * 4096;
      for (size_t v = 0; v < changes.size(); ++v) {
        if (!changes[v]) continue;
        if (modern) disk.read_page(page_id_t::make(page_space_t::CSR_4K, v), image.bytes());
        else legacy.read_page(v, reinterpret_cast<char*>(image.bytes().data()));
        uint32_t actual;
        std::memcpy(&actual, image.bytes().data() + payload, 4);
        if (actual != neighbor(v, 0) + (round * 2 + k + 1) * changes[v])
          throw std::runtime_error("Persisted update mismatch");
      }
      updates[modern].push_back(r);
    }
  }
  print_row("update80/old_pool", updates[0], 0, local.size(), median_seconds(updates[0]));
  print_row("update80/new_pool", updates[1], 0, local.size(), median_seconds(updates[0]));

}
void run_mixed(const options_t& o);
} // namespace
int main(int argc, char** argv) {
  try {
#if defined(BWGRAPH_NEIGHBOR_COMPRESS) || defined(BW_GRAPH_ENABLE_TRANSACTION) || \
    defined(BW_GRAPH_ENABLE_IO_URING)
    throw std::runtime_error("Run with transactions, compression and io_uring off");
#endif
    const auto o = parse(argc, argv);
    std::cout << std::unitbuf;
    std::cout << "loading_bench: seed=20261029, rounds=" << o.rounds << ", depth=" << o.depth
              << "; rates are completed accesses, not SSD bandwidth\n"
                 "case                         status seconds  Kaccess/s   vs.ref"
                 "     reads   writes"
                 "   readMiB      hits coalesced  sec range\n";
    std::cout << "Reference: fixed=old_disk; mixed=new_disk; updates=old_pool; hot=unreferenced.\n";
    if (o.suite != "mixed") run_fixed(o);
    if (o.suite != "fixed") run_mixed(o);
    return 0;
  } catch (const std::overflow_error& e) {
    std::cerr << "loading_bench [fail] " << e.what() << '\n';
  } catch (const YAML::Exception& e) {
    std::cerr << "loading_bench [fail] " << e.what() << '\n';
  } catch (const std::exception& e) {
    std::cerr << "loading_bench [fail] " << e.what() << '\n';
  } catch (...) { std::cerr << "loading_bench [fail] Unrecognized exception\n"; }
  return 1;
}
namespace {
struct fragment_t { uint32_t page, offset, count, slot; };
struct object_t {
  std::vector<uint32_t> neighbors;
  std::vector<fragment_t> old_parts, new_parts;
  uint64_t sum{0};
};
uint64_t consume_native(std::span<const std::byte> bytes, const fragment_t& fragment) {
  multiscale_csr_page_view_t view(bytes);
  auto neighbors = view.neighbors(fragment.slot);
  if (view.page_id() != fragment.page || neighbors.size() != fragment.count)
    throw std::runtime_error("Native vertex record mismatch");
  uint64_t sum = 0;
  for (size_t i = 0; i < neighbors.size(); ++i) sum += neighbors[i];
  return sum;
}
uint64_t consume_legacy(csr_page_t& page, const fragment_t& fragment) {
  auto neighbors = page.get_neighbors(fragment.slot);
  if (neighbors.size() != fragment.count) throw std::runtime_error("Legacy vertex record mismatch");
  return consume(reinterpret_cast<const std::byte*>(neighbors.data()), neighbors.size());
}
void run_mixed(const options_t& o) {
  BW_GRAPH_PAGE_SIZE = 16384;
  BW_GRAPH_MAX_BLOCK_NEIGHBOR = 4;
  BW_GRAPH_GIANT_DEGREE_BOUND = UINT32_MAX;
  BW_BUFFER_PREFETCH_WORKER_COUNT = 4;
  store_t store;
  legacy_disk_t legacy(store.path / "legacy.pages", 16384);
  multiscale_disk_options_t disk_options;
#ifdef __linux__
  disk_options.bypass_cache = false;
#endif
  multiscale_disk_manager_t disk(store.path / "multi", disk_options);
  std::vector<object_t> objects(o.objects);
  std::array<std::vector<size_t>, 4> classes;
  for (size_t i = 0; i < objects.size(); ++i) {
    const size_t bucket = i % 100;
    const size_t c = bucket < 70 ? 0 : bucket < 90 ? 1 : bucket < 98 ? 2 : 3;
    const std::array<size_t, 4> degrees{128, 4096, 32768, 262144};
    auto& object = objects[i];
    object.neighbors.resize(degrees[c]);
    for (size_t e = 0; e < object.neighbors.size(); ++e) {
      object.neighbors[e] = neighbor(i, e);
      object.sum += object.neighbors[e];
    }
    classes[c].push_back(i);
  }
  // Greedy packing on the fixed-page side, including multiple small vertices per page.
  // Oversize vertices are split into adjacency fragments: a loading simulation,
  // not the production RocksDB giant-vertex representation.
  struct piece_t { size_t object, first, count; };
  std::vector<piece_t> pieces;
  uint32_t old_page_count = 0;
  size_t edges = 0;
  const size_t old_header = 16 + 4 * sizeof(page_no_t);
  auto flush_old = [&] {
    if (pieces.empty()) return;
    std::vector<std::byte> bytes(16384);
    const uint16_t vertices = pieces.size(), edge_count = edges;
    const size_t payload = old_header + pieces.size() * sizeof(csr_vertex_t);
    const uint32_t used = payload + edges * 4;
    std::memcpy(bytes.data(), &vertices, 2);
    std::memcpy(bytes.data() + 2, &edge_count, 2);
    std::memcpy(bytes.data() + 4, &used, 4);
    size_t offset = 0;
    for (size_t j = 0; j < pieces.size(); ++j) {
      auto piece = pieces[j];
      csr_vertex_t vertex(piece.object, piece.count, offset);
      std::memcpy(bytes.data() + old_header + j * sizeof(vertex), &vertex, sizeof(vertex));
      std::memcpy(bytes.data() + payload + offset,
                  objects[piece.object].neighbors.data() + piece.first, piece.count * 4);
      objects[piece.object].old_parts.push_back(
          {old_page_count, static_cast<uint32_t>(payload + offset),
           static_cast<uint32_t>(piece.count), static_cast<uint32_t>(j)});
      offset += piece.count * 4;
    }
    legacy.write_page(old_page_count++, reinterpret_cast<const char*>(bytes.data()));
    pieces.clear(); edges = 0;
  };
  for (size_t i = 0; i < objects.size(); ++i) {
    size_t first = 0;
    while (first < objects[i].neighbors.size()) {
      const size_t metadata = old_header + (pieces.size() + 1) * sizeof(csr_vertex_t);
      if (metadata + edges * 4 + 4 > 16384) { flush_old(); continue; }
      const size_t count = std::min(objects[i].neighbors.size() - first,
                                    (16384 - metadata - edges * 4) / 4);
      pieces.push_back({i, first, count}); edges += count; first += count;
    }
  }
  flush_old();
  for (size_t c = 0; c < classes.size(); ++c) {
    const auto space = static_cast<page_space_t>(c);
    const auto size = disk.page_size(space);
    std::vector<multiscale_csr_page_t::vertex_input_t> inputs;
    size_t used = 32;
    auto flush_new = [&] {
      if (inputs.empty()) return;
      const auto id = disk.allocate_page(space);
      auto page = multiscale_csr_page_t::build(size, id.raw(), inputs);
      const auto bytes = page.serialize();
      disk.write_page(id, bytes);
      for (size_t j = 0; j < inputs.size(); ++j) {
        const auto record = page.vertex(j);
        objects[record.vertex_id].new_parts.push_back({id.raw(),
            static_cast<uint32_t>(32 + inputs.size() * 16 + record.neighbor_byte_offset),
            record.degree, static_cast<uint32_t>(j)});
      }
      inputs.clear(); used = 32;
    };
    for (auto i : classes[c]) {
      const auto bytes = 16 + objects[i].neighbors.size() * 4;
      if (used + bytes > size) flush_new();
      inputs.push_back({static_cast<uint32_t>(i), objects[i].neighbors, 0});
      used += bytes;
    }
    flush_new();
  }
  disk.flush();
  multiscale_buf_options_t budgets;
  for (auto& g : budgets.groups) g = {0, 0};
  budgets.groups[0] = {8, 64}; // 2 MiB
  budgets.groups[1] = {8, 24}; // 6 MiB
  budgets.groups[2] = {8, 4};  // 8 MiB
  budgets.groups[3] = {8, 1};  // 16 MiB; total 32 MiB, same as legacy.
  std::cout << "mixed: " << o.objects << " vertices, degree classes 128/4096/32768/262144; "
               "32MiB cache each; new budgets=2/6/8/16MiB; fixed16K packing vs adaptive CSR\n";
  std::vector<std::pair<std::string, std::vector<uint32_t>>> traces;
  std::vector<uint32_t> scan(o.objects);
  std::iota(scan.begin(), scan.end(), 0);
  traces.push_back({"adj_scan", scan});
  std::mt19937 rng(20261029);
  std::vector<uint32_t> local(o.queries);
  const size_t hot_count = std::max<size_t>(100, o.objects / 16);
  for (auto& v : local)
    v = rng() % 100 < 80 ? rng() % hot_count : rng() % o.objects;
  traces.push_back({"adj_local80", local});
  std::vector<uint32_t> frontier;
  for (size_t i = 0; i < o.objects; i += 64)
    for (size_t repeat = 0; repeat < 4; ++repeat)
      for (size_t j = i; j < std::min(i + 64, o.objects); ++j) frontier.push_back(j);
  traces.push_back({"adj_frontier", frontier});
  const std::array<std::string, 8> modes{"old_disk", "old_pool", "old_prefetch", "new_disk",
      "new_pool", "new_disk_batch", "new_batch", "new_prefetch"};
  for (const auto& [name, trace] : traces) {
    uint64_t expected = 0;
    std::vector<fragment_t> old_accesses, new_accesses;
    for (auto v : trace) {
      expected += objects[v].sum;
      old_accesses.insert(old_accesses.end(), objects[v].old_parts.begin(),
                          objects[v].old_parts.end());
      new_accesses.insert(new_accesses.end(), objects[v].new_parts.begin(),
                          objects[v].new_parts.end());
    }
    std::array<std::vector<row_t>, 8> results;
    for (size_t round = 0; round < o.rounds; ++round) {
      for (size_t k = 0; k < modes.size(); ++k) {
        const size_t mode = (k + round) % modes.size();
        const auto& accesses = mode < 3 ? old_accesses : new_accesses;
        std::unique_ptr<buf_pool_t<csr_page_t>> old_pool;
        std::unique_ptr<multiscale_buf_pool_t> pool;
        if (mode == 1 || mode == 2)
          old_pool.reset(buf_pool_t<csr_page_t>::buf_pool_init(8, 256, 16384));
        if (mode == 4 || mode >= 6) pool = std::make_unique<multiscale_buf_pool_t>(disk, budgets);
        // Reusable aligned direct-I/O destinations; allocate outside the measured loop.
        std::vector<disk_page_t> scratch;
        for (size_t c = 0; c < 4; ++c)
          scratch.emplace_back(page_id_t::make(static_cast<page_space_t>(c), 0),
                               mode < 3 ? 16384 : disk.page_size(static_cast<page_space_t>(c)));
        std::unique_ptr<csr_page_t> decoder;
        if (mode == 0) {
          decoder = std::make_unique<csr_page_t>();
          decoder->set_data(reinterpret_cast<char*>(scratch[0].bytes().data()));
        }
        std::vector<disk_page_t> batch_scratch;
        if (mode == 5)
          for (size_t j = 0; j < o.depth; ++j)
            batch_scratch.emplace_back(page_id_t::make(page_space_t::CSR_2M, 0), 2097152);
        row_t r;
        const auto old_reads = legacy.reads.load();
        const auto start = bench_clock::now();
        const size_t lookahead = std::min<size_t>(o.depth, mode == 7 ? 7 : o.depth);
        auto prefetch = [&](size_t i) {
          if (mode == 2) old_pool->buf_page_prefetch(accesses[i].page, &legacy);
          else if (mode == 7) pool->prefetch(page_id_t::from_raw(accesses[i].page));
        };
        if (mode == 2 || mode == 7)
          for (size_t j = 0; j < std::min(lookahead, accesses.size()); ++j) prefetch(j);
        if (mode == 5 || mode == 6) {
          for (size_t i = 0; i < accesses.size();) {
            size_t count = 0;
            std::array<size_t, 4> per_space{};
            std::vector<page_id_t> ids;
            while (count < o.depth && i + count < accesses.size()) {
              const auto id = page_id_t::from_raw(accesses[i + count].page);
              const auto c = static_cast<size_t>(id.space());
              if (per_space[c] ==
                  budgets.groups[c].chunk_count * budgets.groups[c].chunk_size) break;
              ++per_space[c]; ids.push_back(id); ++count;
            }
            // Deduplicate the known frontier in BOTH baselines; caching must beat
            // an equally batched disk reader, not just repeated reads of one page.
            std::vector<page_id_t> unique;
            std::vector<size_t> source_slot;
            for (auto id : ids) {
              auto found = std::find(unique.begin(), unique.end(), id);
              const auto slot = static_cast<size_t>(found - unique.begin());
              source_slot.push_back(slot);
              if (found == unique.end()) unique.push_back(id);
            }
            if (mode == 6) {
              auto pending = pool->fetch_batch(unique);
              for (size_t u = 0; u < unique.size(); ++u) {
                auto guard = pending[u].result.get().read();
                for (size_t j = 0; j < count; ++j)
                  if (source_slot[j] == u)
                    r.checksum += consume_native(guard.bytes(), accesses[i + j]);
              }
            } else {
              std::vector<std::future<void>> pending;
              for (size_t u = 0; u < unique.size(); ++u) {
                auto done = std::make_shared<std::promise<void>>();
                pending.push_back(done->get_future());
                const auto size = disk.page_size(unique[u].space());
                disk.read_into_async(unique[u], batch_scratch[u].bytes().first(size),
                    [done](auto e) {
                  if (e) done->set_exception(e); else done->set_value();
                });
                ++r.reads; r.read_bytes += size;
              }
              for (size_t u = 0; u < unique.size(); ++u) {
                pending[u].get();
                for (size_t j = 0; j < count; ++j)
                  if (source_slot[j] == u)
                    r.checksum += consume_native(batch_scratch[u].bytes().first(
                        disk.page_size(unique[u].space())), accesses[i + j]);
              }
            }
            i += count;
          }
        } else {
          for (size_t i = 0; i < accesses.size(); ++i) {
            const auto a = accesses[i];
            if (mode == 0) {
              legacy.read_page(a.page, reinterpret_cast<char*>(scratch[0].bytes().data()));
              decoder->disable_parse(); decoder->self_parse();
              r.checksum += consume_legacy(*decoder, a);
            } else if (mode == 1 || mode == 2) {
              auto* page = old_pool->buf_page_read(a.page, &legacy);
              r.checksum += consume_legacy(*page, a);
              page->r_unlatch();
            } else if (mode == 3) {
              const auto id = page_id_t::from_raw(a.page);
              auto& data = scratch[static_cast<size_t>(id.space())];
              disk.read_page(id, data.bytes());
              r.checksum += consume_native(data.bytes(), a);
              ++r.reads; r.read_bytes += data.bytes().size();
            } else {
              auto guard = pool->fetch_read(page_id_t::from_raw(a.page));
              r.checksum += consume_native(guard.bytes(), a);
            }
            if ((mode == 2 || mode == 7) && i + lookahead < accesses.size())
              prefetch(i + lookahead);
          }
        }
        if (old_pool) old_pool->wait_for_prefetch_idle();
        r.seconds = std::chrono::duration<double>(bench_clock::now() - start).count();
        if (mode < 3) {
          r.reads = legacy.reads.load() - old_reads; r.read_bytes = r.reads * 16384;
          if (old_pool) r.hits = old_pool->get_stats().cache_hits;
        } else if (pool) {
          for (size_t c = 0; c < 4; ++c) {
            const auto space = static_cast<page_space_t>(c);
            const auto stats = pool->stats(space);
            r.reads += stats.disk_reads; r.read_bytes += stats.disk_reads * disk.page_size(space);
            r.hits += stats.hits; r.coalesced += stats.coalesced;
          }
        }
        results[mode].push_back(r);
      }
    }
    for (size_t mode = 0; mode < modes.size(); ++mode)
      print_row(name + "/" + modes[mode], results[mode], expected, trace.size(),
                median_seconds(results[3]));
  }
  std::cout << "Mixed checksums cover every neighbor. Giant fragments simulate fixed-page loading; "
               "they do not benchmark production RocksDB/SMO/graph algorithms.\n";
}

}
