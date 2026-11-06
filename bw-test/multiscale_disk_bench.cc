#include "bw_graph/storage/multiscale_disk_manager.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <unistd.h>

using namespace bw_graph;

namespace {
using clock_t = std::chrono::steady_clock;
struct options_t {
  uint32_t pages{16384};
  size_t rounds{3};
  size_t workers{4};
  size_t depth{64};
  std::filesystem::path samples;
  bool bypass{true};
  bool keep{false};
  std::filesystem::path parent{std::filesystem::temp_directory_path()};
};

size_t positive(const std::string& text) {
  if (text.empty() || text.front() == '-') throw std::invalid_argument("Expected positive number");
  size_t used = 0;
  const auto value = std::stoull(text, &used);
  if (used != text.size() || value == 0 || value > page_id_t::slot_mask) {
    throw std::invalid_argument("Invalid numeric option");
  }
  return static_cast<size_t>(value);
}

options_t parse(int argc, char** argv) {
  options_t opts;
  // tools/run_tests.sh supplies the repository root before the test's own options.
  int start = 1;
  if (argc > 1 && argv[1][0] != '-') {
    if (!std::filesystem::is_regular_file(std::filesystem::path(argv[1]) / "CMakeLists.txt")) {
      throw std::invalid_argument("Unexpected positional argument");
    }
    start = 2;
  }
  for (int i = start; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--buffered") { opts.bypass = false; continue; }
    if (key == "--keep") { opts.keep = true; continue; }
    if (key == "--help") {
      std::cout << "multiscale_disk_bench [--pages N] [--rounds N] [--workers N] "
                   "[--depth N] [--directory PARENT] [--samples CSV] [--buffered] [--keep]\n";
      std::exit(0);
    }
    if (++i == argc) throw std::invalid_argument("Missing option value");
    if (key == "--directory") opts.parent = argv[i];
    else if (key == "--samples") opts.samples = argv[i];
    else if (key == "--pages") opts.pages = static_cast<uint32_t>(positive(argv[i]));
    else if (key == "--rounds") opts.rounds = positive(argv[i]);
    else if (key == "--workers") opts.workers = positive(argv[i]);
    else if (key == "--depth") opts.depth = positive(argv[i]);
    else throw std::invalid_argument("Unknown option: " + key);
  }
  return opts;
}

struct temporary_store_t {
  explicit temporary_store_t(const options_t& opts) : keep(opts.keep) {
    std::filesystem::create_directories(opts.parent);
    std::string pattern = (opts.parent / "bw-disk-bench-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
      throw std::runtime_error("Cannot create benchmark dir");
    }
    path = pattern;
  }
  ~temporary_store_t() {
    if (!keep) {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  }
  std::filesystem::path path;
  bool keep;
};

uint64_t marker(page_id_t id) { return 0xbadc0ffe00000000ULL | id.raw(); }
void stamp(disk_page_t& scratch, page_id_t id) {
  const auto value = marker(id);
  std::memcpy(scratch.bytes().data(), &value, sizeof(value));
  std::memcpy(scratch.bytes().data() + 4088, &value, sizeof(value));
}
void verify(const disk_page_t& page, page_id_t expected) {
  uint64_t first, last;
  std::memcpy(&first, page.bytes().data(), 8);
  std::memcpy(&last, page.bytes().data() + 4088, 8);
  if (page.page_id() != expected || first != marker(expected) || last != first ||
      !std::all_of(page.bytes().begin() + 8, page.bytes().end() - 8,
                   [](std::byte b) { return b == std::byte{0xa5}; })) {
    throw std::runtime_error("Page data mismatch");
  }
}

void read_work(multiscale_disk_manager_t& disk, const std::vector<page_id_t>& ids,
               size_t depth, unsigned mode) {
  if (mode == 0) {
    for (auto id : ids) verify(disk.read_page(id), id);
  } else if (mode == 1) {
    for (size_t i = 0; i < ids.size(); i += depth) {
      auto chunk = std::span(ids).subspan(i, std::min(depth, ids.size() - i));
      auto pages = disk.read_pages(chunk);
      for (size_t j = 0; j < pages.size(); ++j) verify(pages[j], chunk[j]);
    }
  } else if (mode == 2) {
    // Single-request API, depth outstanding futures; no thread per request.
    std::deque<page_read_request_t> pending;
    size_t next = 0;
    while (next < ids.size() || !pending.empty()) {
      while (next < ids.size() && pending.size() < depth) {
        pending.push_back(disk.read_page_async(ids[next++]));
      }
      auto request = std::move(pending.front());
      pending.pop_front();
      verify(request.result.get(), request.page_id);
    }
  } else {
    // Double-buffer batches: submit the next batch before consuming the current one.
    // depth is the TOTAL outstanding request limit, split into two halves.
    const size_t batch_size = std::max(size_t{1}, depth / 2);
    size_t next = 0;
    auto submit = [&] {
      auto chunk = std::span(ids).subspan(next, std::min(batch_size, ids.size() - next));
      next += chunk.size();
      return disk.read_pages_async(chunk);
    };
    auto current = submit();
    while (!current.empty()) {
      auto following = depth > 1 ? submit() : std::vector<page_read_request_t>{};
      for (auto& request : current) verify(request.result.get(), request.page_id);
      current = depth > 1 ? std::move(following) : submit();
    }
  }
}

template <class Function>
void run_case(const char* name, const options_t& opts, std::ostream* samples, Function&& fn,
              const std::function<void()>& check = {}) {
  std::vector<double> seconds;
  for (size_t round = 0; round < opts.rounds; ++round) {
    const auto begin = clock_t::now();
    fn();
    seconds.push_back(std::chrono::duration<double>(clock_t::now() - begin).count());
    if (check) check(); // Write verification is outside timing, before reporting pass.
    if (samples) {
      *samples << name << ',' << round + 1 << ',' << opts.pages << ',' << std::setprecision(9)
               << seconds.back() << ',' << opts.pages / seconds.back() << ','
               << double(opts.pages) / 256 / seconds.back() << '\n';
      samples->flush();
      if (!*samples) throw std::runtime_error("Cannot write benchmark samples");
    }
  }
  std::sort(seconds.begin(), seconds.end());
  const double median = seconds.size() % 2 ? seconds[seconds.size() / 2] :
      (seconds[seconds.size() / 2 - 1] + seconds[seconds.size() / 2]) / 2;
  const double mib = double(opts.pages) / 256;
  std::cout << std::left << std::setw(25) << name << " [ pass ] " << std::right
            << std::fixed << std::setprecision(2) << std::setw(8) << median << " s "
            << std::setprecision(0) << std::setw(9) << opts.pages / median << " IOPS "
            << std::setprecision(1) << std::setw(8) << mib / median << " MiB/s ["
            << mib / seconds.back() << ", " << mib / seconds.front() << "]\n" << std::flush;
}
} // namespace

int main(int argc, char** argv) {
  try {
    const auto opts = parse(argc, argv);
    std::ofstream samples;
    if (!opts.samples.empty()) {
      if (std::filesystem::exists(opts.samples)) {
        throw std::invalid_argument("Samples file already exists; choose a new path");
      }
      samples.open(opts.samples);
      if (!samples) throw std::runtime_error("Cannot create samples file");
      samples << "case,round,pages,seconds,iops,mib_per_second\n";
    }
    auto* sample_out = opts.samples.empty() ? nullptr : &samples;
    temporary_store_t store(opts);
    multiscale_disk_options_t disk_opts;
    disk_opts.io_workers = opts.workers;
    disk_opts.queue_capacity = opts.depth;
    disk_opts.bypass_cache = opts.bypass;
    multiscale_disk_manager_t disk(store.path, disk_opts);
    std::vector<page_id_t> seq;
    seq.reserve(opts.pages);
    for (uint32_t i = 0; i < opts.pages; ++i) {
      seq.push_back(disk.allocate_page(page_space_t::CSR_4K));
    }
    auto random = seq;
    std::mt19937 rng(20261028);
    std::shuffle(random.begin(), random.end(), rng);
    disk_page_t scratch(seq.front(), 4096);
    std::fill(scratch.bytes().begin(), scratch.bytes().end(), std::byte{0xa5});
    auto write = [&](const std::vector<page_id_t>& ids) {
      for (auto id : ids) { stamp(scratch, id); disk.write_page(id, scratch.bytes()); }
      disk.flush(); // Include end-of-phase durability cost, not one fsync per page.
    };
#ifdef __APPLE__
    const char* uncached = "F_NOCACHE";
#else
    const char* uncached = "O_DIRECT";
#endif
#ifdef NDEBUG
    const char* build = "Release-like";
#else
    const char* build = "Debug (not performance evidence)";
#endif
    std::cout << "multiscale_disk: " << opts.pages << " x 4KiB, rounds=" << opts.rounds
              << ", workers=" << opts.workers << ", depth=" << opts.depth << ", "
              << (opts.bypass ? uncached : "buffered/warm") << ", " << build << '\n'
              << "path=" << store.path.string() << " seed=20261028\n"
              << "median + [min,max] MiB/s; writes include flush; reads include validation.\n"
              << std::flush;
    write(seq); // Allocation and initial materialization are outside measured overwrite phases.
    auto verify_written = [&] { for (auto id : seq) verify(disk.read_page(id), id); };
    run_case("write.seq.4k", opts, sample_out, [&] { write(seq); }, verify_written);
    run_case("write.random.4k", opts, sample_out, [&] { write(random); }, verify_written);
    const std::array<const char*, 8> names{
        "read.seq.sync", "read.random.sync", "read.seq.batch", "read.random.batch",
        "read.seq.async", "read.random.async", "read.seq.async_batch", "read.random.async_batch"};
    for (unsigned mode = 0; mode < 4; ++mode) {
      run_case(names[mode * 2], opts, sample_out, [&] { read_work(disk, seq, opts.depth, mode); });
      run_case(names[mode * 2 + 1], opts, sample_out,
               [&] { read_work(disk, random, opts.depth, mode); });
    }
    disk.shutdown();
    std::cout << "10 cases [ pass ]; data " << (opts.keep ? "retained" : "removed") << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "multiscale_disk [ fail ] " << e.what() << '\n';
    return 1;
  }
}
