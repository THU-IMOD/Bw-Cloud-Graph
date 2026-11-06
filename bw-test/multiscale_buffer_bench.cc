#include "bw_graph/buf/multiscale_buf_pool.h"
#include "bw_graph/common/config.h"

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
  size_t pages{8192}, frames{128}, rounds{3}, workers{4}, depth{16}, hot_ops{1000000};
  bool bypass{true};
  std::filesystem::path parent{std::filesystem::temp_directory_path()}, config;
};
size_t positive(const std::string& text) {
  const auto value = parse_buffer_count(YAML::Node(text));
  if (!value || value > page_id_t::slot_mask) throw std::invalid_argument("Invalid option value");
  return value;
}
options_t parse(int argc, char** argv) {
  options_t options;
  int start = 1;
  if (argc > 1 && argv[1][0] != '-') {
    if (!std::filesystem::is_regular_file(std::filesystem::path(argv[1]) / "CMakeLists.txt"))
      throw std::invalid_argument("Unexpected positional argument");
    start = 2;
  }
  for (int i = start; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--buffered") { options.bypass = false; continue; }
    if (key == "--help") {
      std::cout << "multiscale_buffer_bench [--pages N] [--frames N] [--rounds N] "
          "[--workers N] [--depth N] [--hot-ops N] [--config YAML] [--directory PARENT] "
          "[--buffered]\n";
      std::exit(0);
    }
    if (++i == argc) throw std::invalid_argument("Missing option value");
    if (key == "--pages") options.pages = positive(argv[i]);
    else if (key == "--frames") options.frames = positive(argv[i]);
    else if (key == "--rounds") options.rounds = positive(argv[i]);
    else if (key == "--workers") options.workers = positive(argv[i]);
    else if (key == "--depth") options.depth = positive(argv[i]);
    else if (key == "--hot-ops") options.hot_ops = positive(argv[i]);
    else if (key == "--directory") options.parent = argv[i];
    else if (key == "--config") options.config = argv[i];
    else throw std::invalid_argument("Unknown option: " + key);
  }
  if (options.pages < 8) throw std::invalid_argument("Use at least eight 4 KiB pages");
  return options;
}
struct temporary_store_t {
  std::filesystem::path path;
  explicit temporary_store_t(const std::filesystem::path& parent) {
    std::string pattern = (parent / "bw-buffer-bench-XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw std::runtime_error("Cannot create benchmark directory");
    path = pattern;
  }
  ~temporary_store_t() { std::error_code error; std::filesystem::remove_all(path, error); }
};
uint64_t marker(page_id_t id) { return 0xbadc0ffe00000000ULL | id.raw(); }
void stamp(std::span<std::byte> bytes, page_id_t id) {
  const auto value = marker(id);
  std::memcpy(bytes.data(), &value, 8);
  std::memcpy(bytes.data() + bytes.size() - 8, &value, 8);
}
void verify(std::span<const std::byte> bytes, page_id_t id) {
  uint64_t first, last;
  std::memcpy(&first, bytes.data(), 8);
  std::memcpy(&last, bytes.data() + bytes.size() - 8, 8);
  if (first != marker(id) || last != marker(id)) throw std::runtime_error("Page marker mismatch");
}
struct result_t {
  double seconds{0};
  uint64_t ops{0}, bytes{0}, hits{0}, reads{0}, writes{0};
};
template<class F> void report(const std::string& name, size_t rounds, F&& run) {
  std::vector<result_t> samples;
  for (size_t i = 0; i < rounds; ++i) samples.push_back(run(i));
  std::sort(samples.begin(), samples.end(), [](auto a, auto b) { return a.seconds < b.seconds; });
  const auto r = samples[samples.size() / 2];
  const double rate = static_cast<double>(r.bytes) / (1024 * 1024);
  std::cout << std::left << std::setw(26) << name << " [pass] " << std::right
            << std::fixed << std::setprecision(3) << std::setw(8) << r.seconds
            << std::setprecision(2) << std::setw(9) << r.ops / r.seconds / 1e6
            << std::setprecision(1) << std::setw(10) << rate / r.seconds
            << std::setw(8) << 100.0 * r.hits / r.ops
            << std::setw(9) << r.reads << std::setw(9) << r.writes
            << "  " << rate / samples.back().seconds << ".."
            << rate / samples.front().seconds << '\n';
}
result_t measured(bench_clock::time_point start, size_t operations, uint32_t page_size,
                  buffer_group_stats_t before = {}, buffer_group_stats_t after = {}) {
  return {std::chrono::duration<double>(bench_clock::now() - start).count(), operations,
      uint64_t{operations} * page_size, after.hits - before.hits,
      after.disk_reads - before.disk_reads, after.writebacks - before.writebacks};
}
} // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse(argc, argv);
    std::cout << std::unitbuf;
    temporary_store_t store(options.parent);
    multiscale_disk_options_t disk_options;
    disk_options.bypass_cache = options.bypass;
    disk_options.io_workers = options.workers;
    multiscale_buf_options_t budgets;
    if (!options.config.empty()) {
      load_yaml_config(options.config.string());
      budgets = BW_MULTISCALE_BUFFER_CONFIG;
      if (BW_GRAPH_INDEX_PAGE_SIZE > UINT32_MAX || BW_DELTA_PAGE_SIZE > UINT32_MAX)
        throw std::invalid_argument("Index/Delta page size exceeds disk option range");
      disk_options.index_page_size = static_cast<uint32_t>(BW_GRAPH_INDEX_PAGE_SIZE);
      disk_options.delta_page_size = static_cast<uint32_t>(BW_DELTA_PAGE_SIZE);
    }
    multiscale_disk_manager_t disk(store.path, disk_options);
    std::array<std::vector<page_id_t>, 6> sequential, random;
    const std::array<const char*, 6> names{"4k", "32k", "256k", "2m", "index", "delta"};
    for (size_t s = 0; s < 6; ++s) {
      const auto space = static_cast<page_space_t>(s);
      const auto size = disk.page_size(space);
      const auto count = std::max<size_t>(8, options.pages * uint64_t{4096} / size);
      if (count > page_id_t::slot_mask) throw std::invalid_argument("Too many pages");
      if (options.config.empty())
        budgets.groups[s] = {1, std::max<size_t>(2, options.frames * uint64_t{4096} / size)};
      const auto frames = budgets.groups[s].chunk_count * budgets.groups[s].chunk_size;
      if (!frames || frames >= count)
        throw std::invalid_argument("Each group needs 1 <= frames < dataset pages for eviction");
      disk_page_t scratch(page_id_t::make(space, 0), size);
      std::fill(scratch.bytes().begin(), scratch.bytes().end(), std::byte{0xa5});
      for (size_t i = 0; i < count; ++i) {
        const auto id = disk.allocate_page(space);
        sequential[s].push_back(id);
        stamp(scratch.bytes(), id);
        disk.write_page(id, scratch.bytes());
      }
      random[s] = sequential[s];
      std::mt19937 rng(20261029);
      std::shuffle(random[s].begin(), random[s].end(), rng);
    }
    disk.flush();
    std::cout << "multiscale_buffer_bench: " << options.pages << " x 4KiB-equivalent/space, "
              << options.rounds << " rounds, workers=" << options.workers
              << ", bypass=" << options.bypass << '\n';
    std::cout << "frame/depth:";
    for (size_t s = 0; s < 6; ++s) {
      const auto frames = budgets.groups[s].chunk_count * budgets.groups[s].chunk_size;
      std::cout << ' ' << names[s] << '=' << frames << '/' << std::min(options.depth, frames);
    }
    std::cout << "\ncase                         status  seconds   Mpage/s     MiB/s    hit%"
                 "    io-read io-write  MiB/s range\n";
    for (size_t s = 0; s < 6; ++s) {
      const auto space = static_cast<page_space_t>(s);
      const auto size = disk.page_size(space);
      const auto frames = budgets.groups[s].chunk_count * budgets.groups[s].chunk_size;
      const auto depth = std::min(options.depth, frames);
      for (bool shuffled : {false, true}) {
        if (s != 0 && !shuffled) continue;
        const auto& ids = shuffled ? random[s] : sequential[s];
        const std::string suffix = std::string(shuffled ? "_rand_" : "_seq_") + names[s];
        // Reusable aligned destination: baseline excludes page allocation/copy too.
        auto direct_sync = [&](size_t) {
          disk_page_t scratch(ids.front(), size);
          const auto start = bench_clock::now();
          for (auto id : ids) { disk.read_page(id, scratch.bytes()); verify(scratch.bytes(), id); }
          auto result = measured(start, ids.size(), size);
          result.reads = ids.size();
          return result;
        };
        auto pool_sync = [&](size_t) {
          multiscale_buf_pool_t pool(disk, budgets);
          const auto start = bench_clock::now();
          for (auto id : ids) { auto guard = pool.fetch_read(id); verify(guard.bytes(), id); }
          auto result = measured(start, ids.size(), size, {}, pool.stats(space));
          if (result.reads != ids.size() || result.hits) throw std::runtime_error("Not cold");
          return result;
        };
        // Alternate pair order between access patterns to reduce systematic ordering bias.
        if (s == 0 && shuffled) {
          report("pool_sync" + suffix, options.rounds, pool_sync);
          report("disk_sync" + suffix, options.rounds, direct_sync);
        } else if (s == 0) {
          report("disk_sync" + suffix, options.rounds, direct_sync);
          report("pool_sync" + suffix, options.rounds, pool_sync);
        }
        auto disk_async = [&](size_t) {
          std::vector<disk_page_t> scratch;
          for (size_t j = 0; j < depth; ++j) scratch.emplace_back(ids.front(), size);
          const auto start = bench_clock::now();
          for (size_t offset = 0; offset < ids.size(); offset += depth) {
            const auto count = std::min(depth, ids.size() - offset);
            std::vector<std::future<void>> pending;
            for (size_t j = 0; j < count; ++j) {
              auto done = std::make_shared<std::promise<void>>();
              pending.push_back(done->get_future());
              disk.read_into_async(ids[offset + j], scratch[j].bytes(), [done](auto error) {
                if (error) done->set_exception(error); else done->set_value();
              });
            }
            for (size_t j = 0; j < count; ++j) {
              pending[j].get(); verify(scratch[j].bytes(), ids[offset + j]);
            }
          }
          auto result = measured(start, ids.size(), size); result.reads = ids.size(); return result;
        };
        auto pool_async = [&](size_t) {
          multiscale_buf_pool_t pool(disk, budgets);
          const auto start = bench_clock::now();
          for (size_t offset = 0; offset < ids.size(); offset += depth) {
            const auto count = std::min(depth, ids.size() - offset);
            auto requests = pool.fetch_batch(std::span(ids).subspan(offset, count));
            for (auto& request : requests) {
              auto guard = request.result.get().read(); verify(guard.bytes(), request.page_id);
            }
          }
          auto result = measured(start, ids.size(), size, {}, pool.stats(space));
          if (result.reads != ids.size() || result.hits) throw std::runtime_error("Not cold");
          return result;
        };
        report("disk_async" + suffix, options.rounds, disk_async);
        report("pool_async" + suffix, options.rounds, pool_async);
      }
    }
    // Hot-path tests touch first/last markers only, not every byte of each page.
    for (size_t threads : {size_t{1}, size_t{4}}) {
      report("pool_hot_read_4k_t" + std::to_string(threads), options.rounds, [&](size_t) {
        multiscale_buf_pool_t pool(disk, budgets);
        const auto frames = budgets.groups[0].chunk_count * budgets.groups[0].chunk_size;
        for (size_t j = 0; j < frames; ++j) (void)pool.fetch_read(sequential[0][j]);
        const auto before = pool.stats(page_space_t::CSR_4K);
        std::barrier gate(static_cast<ptrdiff_t>(threads + 1));
        std::vector<std::thread> readers;
        std::vector<std::exception_ptr> errors(threads);
        for (size_t t = 0; t < threads; ++t) readers.emplace_back([&, t] {
          gate.arrive_and_wait();
          try {
            for (size_t j = t; j < options.hot_ops; j += threads) {
              const auto id = sequential[0][j % frames];
              auto guard = pool.fetch_read(id); verify(guard.bytes(), id);
            }
          } catch (...) { errors[t] = std::current_exception(); }
        });
        const auto start = bench_clock::now();
        gate.arrive_and_wait();
        for (auto& reader : readers) reader.join();
        for (auto error : errors) if (error) std::rethrow_exception(error);
        return measured(start, options.hot_ops, 4096, before,
                        pool.stats(page_space_t::CSR_4K));
      });
    }
    report("pool_hot_write_4k", options.rounds, [&](size_t) {
      multiscale_buf_pool_t pool(disk, budgets);
      const auto frames = budgets.groups[0].chunk_count * budgets.groups[0].chunk_size;
      for (size_t j = 0; j < frames; ++j) (void)pool.fetch_read(sequential[0][j]);
      const auto before = pool.stats(page_space_t::CSR_4K);
      const auto start = bench_clock::now();
      for (size_t j = 0; j < options.hot_ops; ++j) {
        const auto id = sequential[0][j % frames];
        auto guard = pool.fetch_write(id); stamp(guard.bytes(), id);
      }
      auto result = measured(start, options.hot_ops, 4096, before,
                             pool.stats(page_space_t::CSR_4K));
      pool.flush_all(); // Hot-write timing is memory-only; persistent path measured next.
      return result;
    });
    report("disk_update_4k", options.rounds, [&](size_t round) {
      disk_page_t scratch(random[0].front(), 4096);
      const auto start = bench_clock::now();
      for (auto id : random[0]) {
        disk.read_page(id, scratch.bytes()); verify(scratch.bytes(), id);
        const uint64_t value = round + 1;
        std::memcpy(scratch.bytes().data() + 8, &value, 8);
        disk.write_page(id, scratch.bytes());
      }
      disk.flush();
      auto result = measured(start, random[0].size(), 4096);
      result.reads = result.writes = random[0].size();
      return result;
    });
    report("pool_dirty_evict_4k", options.rounds, [&](size_t round) {
      multiscale_buf_pool_t pool(disk, budgets);
      const auto start = bench_clock::now();
      for (auto id : random[0]) {
        auto guard = pool.fetch_write(id); verify(guard.bytes(), id);
        const uint64_t value = round + 101;
        std::memcpy(guard.bytes().data() + 8, &value, 8);
      }
      pool.flush_all(); // Includes reads, writes, and final durable flush.
      auto result = measured(start, random[0].size(), 4096, {},
                             pool.stats(page_space_t::CSR_4K));
      disk_page_t scratch(random[0].front(), 4096);
      for (auto id : random[0]) {
        disk.read_page(id, scratch.bytes());
        uint64_t value; std::memcpy(&value, scratch.bytes().data() + 8, 8);
        if (value != round + 101) throw std::runtime_error("Writeback mismatch");
      }
      return result;
    });
    // Reopen after shutdown: verify persisted markers from every space.
    disk.shutdown();
    multiscale_disk_manager_t reopened(store.path, disk_options);
    for (size_t s = 0; s < 6; ++s) {
      auto first = reopened.read_page(sequential[s].front()); verify(first.bytes(), first.page_id());
      auto last = reopened.read_page(sequential[s].back()); verify(last.bytes(), last.page_id());
    }
    std::cout << "reopen                     [pass]\n"
                 "MiB/s = logical page bytes; hot rows touch markers only. "
                 "Cold rows use fresh pools; SSD caches are not reset.\n";
    return 0;
  } catch (const std::overflow_error& error) {
    // Explicit library-type catches also work with mixed macOS dependency RTTI,
    // where the std::exception base catch can fail to match a library exception.
    std::cerr << "multiscale_buffer_bench [fail] " << error.what() << '\n';
    return 1;
  } catch (const YAML::Exception& error) {
    std::cerr << "multiscale_buffer_bench [fail] " << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "multiscale_buffer_bench [fail] " << error.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "multiscale_buffer_bench [fail] Unrecognized exception\n";
    return 1;
  }
}
