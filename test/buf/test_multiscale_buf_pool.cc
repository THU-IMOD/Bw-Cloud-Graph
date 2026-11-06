#include "bw_graph/buf/multiscale_buf_pool.h"
#include "bw_graph/io/multiscale_csr_page_view.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <unistd.h>

using namespace bw_graph;
namespace {
class MultiscaleBufferTest : public ::testing::TestWithParam<bool> {
protected:
  void SetUp() override {
    char path[] = "/tmp/bw-buffer-test-XXXXXX";
    const char* created = mkdtemp(path);
    ASSERT_NE(created, nullptr);
    directory = created;
    disk_options.bypass_cache = GetParam();
    disk_options.io_workers = 1;
    disk_options.queue_capacity = 32;
    for (auto& g : options.groups) g = {1, 2};
  }
  void TearDown() override { std::filesystem::remove_all(directory); }
  std::filesystem::path directory;
  multiscale_disk_options_t disk_options;
  multiscale_buf_options_t options;
};

TEST_P(MultiscaleBufferTest, TryPinDoesNotReadOrReserveOnMiss) {
  multiscale_disk_manager_t disk(directory, disk_options);
  multiscale_buf_pool_t pool(disk, options);
  auto id = disk.allocate_page(page_space_t::CSR_4K);
  EXPECT_FALSE(pool.try_pin_page(id));
  EXPECT_EQ(pool.stats(page_space_t::CSR_4K).disk_reads, 0);
  EXPECT_EQ(pool.stats(page_space_t::CSR_4K).pinned_frames, 0);
  { auto guard = pool.fetch_read(id); }
  {
    auto pin = pool.try_pin_page(id);
    ASSERT_TRUE(pin);
    EXPECT_EQ(pin.page_id(), id);
    EXPECT_EQ(pool.stats(page_space_t::CSR_4K).disk_reads, 1);
    EXPECT_EQ(pool.stats(page_space_t::CSR_4K).pinned_frames, 1);
  }
  EXPECT_EQ(pool.stats(page_space_t::CSR_4K).pinned_frames, 0);
  EXPECT_TRUE(pool.close());
  EXPECT_THROW(pool.try_pin_page(id), std::runtime_error);
}

TEST_P(MultiscaleBufferTest, SixSpacesChunksHitsWritebackAndReopen) {
  std::array<page_id_t, 6> ids;
  {
    multiscale_disk_manager_t disk(directory, disk_options);
    multiscale_buf_pool_t pool(disk, options);
    for (size_t i = 0; i < 6; ++i) {
      const auto space = static_cast<page_space_t>(i);
      ids[i] = disk.allocate_page(space);
      const auto second = disk.allocate_page(space);
      {
        auto a = pool.fetch_write(ids[i]);
        auto b = pool.fetch_write(second);
        EXPECT_EQ(a.bytes().size(), disk.page_size(space));
        const auto first_address = reinterpret_cast<uintptr_t>(a.bytes().data());
        const auto second_address = reinterpret_cast<uintptr_t>(b.bytes().data());
        EXPECT_EQ(first_address % 4096, 0u);
        EXPECT_EQ(second_address % 4096, 0u);
        EXPECT_EQ(std::max(first_address, second_address) -
                  std::min(first_address, second_address), disk.page_size(space));
        std::fill(a.bytes().begin(), a.bytes().end(), std::byte(i + 1));
        std::fill(b.bytes().begin(), b.bytes().end(), std::byte(i + 11));
        EXPECT_EQ(pool.stats(space).pinned_frames, 2u);
      }
      {
        auto a = pool.fetch_read(ids[i]);
        EXPECT_EQ(a.bytes().back(), std::byte(i + 1));
      }
      EXPECT_EQ(pool.stats(space).disk_reads, 2u);
      EXPECT_EQ(pool.stats(space).hits, 1u);
      EXPECT_EQ(pool.stats(space).frame_count, 2u);
      EXPECT_GT(pool.stats(space).chunk_bytes, size_t{2} * disk.page_size(space));
    }
    EXPECT_TRUE(pool.close());
    EXPECT_THROW(pool.fetch_read(ids[0]), std::runtime_error);
  }
  multiscale_disk_manager_t disk(directory, disk_options);
  for (size_t i = 0; i < 6; ++i) {
    const auto data = disk.read_page(ids[i]);
    EXPECT_TRUE(std::all_of(data.bytes().begin(), data.bytes().end(),
                            [i](auto b) { return b == std::byte(i + 1); }));
  }
}

TEST_P(MultiscaleBufferTest, PinsCapacityBatchErrorsAndCloseRetry) {
  options.groups[0] = {1, 1};
  multiscale_disk_manager_t disk(directory, disk_options);
  const auto a = disk.allocate_page(page_space_t::CSR_4K);
  const auto b = disk.allocate_page(page_space_t::CSR_4K);
  multiscale_buf_pool_t pool(disk, options);
  auto held = pool.fetch_async(a);
  held.result.wait(); // Unconsumed results must keep the frame pinned.
  EXPECT_EQ(pool.stats(a.space()).pinned_frames, 1u);
  const std::array ids{a, b};
  auto requests = pool.fetch_batch(ids);
  EXPECT_THROW((void)requests[1].result.get(), buffer_full_t);
  { auto guard = requests[0].result.get().read(); EXPECT_EQ(guard.page_id(), a); }
  EXPECT_FALSE(pool.close());
  { auto guard = held.result.get().write(); guard.bytes()[0] = std::byte{77}; }
  EXPECT_TRUE(pool.close());
  EXPECT_EQ(disk.read_page(a).bytes()[0], std::byte{77});
}

TEST_P(MultiscaleBufferTest, CoalescesMissesAndDiscardedResultsReleasePins) {
  multiscale_disk_manager_t disk(directory, disk_options);
  const auto id = disk.allocate_page(page_space_t::CSR_2M);
  disk_page_t blocker(id, disk.page_size(id.space()));
  std::promise<void> entered, release;
  auto release_future = release.get_future().share();
  disk.read_into_async(id, blocker.bytes(), [&](std::exception_ptr error) {
    entered.set_value();
    release_future.wait(); // Test-only external gate, never waits for an executor job.
    EXPECT_FALSE(error);
  });
  entered.get_future().wait();
  // Always release the worker even if an assertion/exception exits early.
  struct release_t { std::promise<void>& p; ~release_t() { p.set_value(); } };
  multiscale_buf_pool_t pool(disk, options);
  std::vector<buffer_read_request_t> requests;
  {
    release_t gate{release};
    for (size_t i = 0; i < 16; ++i) requests.push_back(pool.fetch_async(id));
    EXPECT_EQ(pool.stats(id.space()).coalesced, 15u);
    EXPECT_EQ(pool.stats(id.space()).disk_reads, 0u);
    requests.erase(requests.begin() + 1, requests.end());
  }
  { auto guard = requests[0].result.get().read(); EXPECT_EQ(guard.bytes().size(), 2097152u); }
  // Completion publishes each waiter outside the metadata lock. Drain the executor
  // before asserting all abandoned result pins have been destroyed.
  (void)disk.read_page_async(id).result.get();
  EXPECT_EQ(pool.stats(id.space()).disk_reads, 1u);
  EXPECT_EQ(pool.stats(id.space()).pinned_frames, 0u);
  EXPECT_TRUE(pool.close());
}

TEST_P(MultiscaleBufferTest, DirtyEvictionAndReadFailureRecovery) {
  options.groups[0] = {1, 1};
  multiscale_disk_manager_t disk(directory, disk_options);
  const auto a = disk.allocate_page(page_space_t::CSR_4K);
  const auto b = disk.allocate_page(page_space_t::CSR_4K);
  multiscale_buf_pool_t pool(disk, options);
  { auto guard = pool.fetch_write(a); guard.bytes().back() = std::byte{42}; }
  { auto guard = pool.fetch_read(b); EXPECT_EQ(guard.bytes().back(), std::byte{0}); }
  EXPECT_EQ(disk.read_page(a).bytes().back(), std::byte{42});
  const auto nonexistent = page_id_t::make(a.space(), 100);
  EXPECT_THROW((void)pool.fetch_async(nonexistent).result.get(), std::out_of_range);
  EXPECT_EQ(pool.stats(a.space()).pinned_frames, 0u);
  { auto guard = pool.fetch_read(a); EXPECT_EQ(guard.bytes().back(), std::byte{42}); }
  EXPECT_EQ(pool.stats(a.space()).writebacks, 1u);
  EXPECT_EQ(pool.stats(a.space()).failures, 1u);
}

TEST_P(MultiscaleBufferTest, FailedDirtyWritePreservesResidentData) {
  options.groups[0] = {1, 1};
  multiscale_disk_manager_t disk(directory, disk_options);
  const auto a = disk.allocate_page(page_space_t::CSR_4K);
  const auto b = disk.allocate_page(page_space_t::CSR_4K);
  multiscale_buf_pool_t pool(disk, options);
  { auto guard = pool.fetch_write(a); guard.bytes()[10] = std::byte{63}; }
  disk.shutdown(); // Deterministic write failure without corrupting arbitrary files.
  EXPECT_THROW(pool.fetch_read(b), std::runtime_error);
  { auto guard = pool.fetch_read(a); EXPECT_EQ(guard.bytes()[10], std::byte{63}); }
  EXPECT_EQ(pool.stats(a.space()).disk_reads, 1u);
  EXPECT_EQ(pool.stats(a.space()).pinned_frames, 0u);
  EXPECT_THROW(pool.close(), std::runtime_error); // Must not report successful persistence.
}

TEST_P(MultiscaleBufferTest, ConcurrentWritersAndReaders) {
  multiscale_disk_manager_t disk(directory, disk_options);
  const auto id = disk.allocate_page(page_space_t::CSR_4K);
  multiscale_buf_pool_t pool(disk, options);
  std::atomic<bool> failed{false};
  std::vector<std::thread> threads;
  for (size_t i = 0; i < 4; ++i) threads.emplace_back([&] {
    try {
      for (size_t j = 0; j < 2000; ++j) {
        auto guard = pool.fetch_write(id);
        uint64_t value;
        std::memcpy(&value, guard.bytes().data(), sizeof(value));
        ++value;
        std::memcpy(guard.bytes().data(), &value, sizeof(value));
        std::memcpy(guard.bytes().data() + 8, &value, sizeof(value));
      }
    } catch (...) { failed = true; }
  });
  for (size_t i = 0; i < 2; ++i) threads.emplace_back([&] {
    try {
      for (size_t j = 0; j < 2000; ++j) {
        auto guard = pool.fetch_read(id);
        uint64_t first, second;
        std::memcpy(&first, guard.bytes().data(), 8);
        std::memcpy(&second, guard.bytes().data() + 8, 8);
        if (first != second) failed = true;
      }
    } catch (...) { failed = true; }
  });
  for (auto& thread : threads) thread.join();
  EXPECT_FALSE(failed);
  pool.flush_all();
  uint64_t value;
  auto page = disk.read_page(id);
  std::memcpy(&value, page.bytes().data(), 8);
  EXPECT_EQ(value, 8000u);
  EXPECT_EQ(pool.stats(id.space()).disk_reads, 1u);
}

TEST_P(MultiscaleBufferTest, BorrowedCsrViewAndBounds) {
  multiscale_disk_manager_t disk(directory, disk_options);
  multiscale_buf_pool_t pool(disk, options);
  for (size_t i = 0; i < 4; ++i) {
    auto id = disk.allocate_page(static_cast<page_space_t>(i));
    const std::array<uint32_t, 3> neighbors{3, 8, 11};
    const std::array inputs{multiscale_csr_page_t::vertex_input_t{42, neighbors, 5}};
    auto serialized = multiscale_csr_page_t::build(disk.page_size(id.space()), id.raw(), inputs)
                          .serialize();
    { auto guard = pool.fetch_write(id);
      std::memcpy(guard.bytes().data(), serialized.data(), serialized.size()); }
    { auto guard = pool.fetch_read(id);
      multiscale_csr_page_view_t view(guard.bytes());
      EXPECT_EQ(view.page_id(), id.raw());
      EXPECT_EQ(view.vertex_count(), 1u);
      EXPECT_EQ(view.vertex(0).vertex_id, 42u);
      const auto list = view.neighbors(0);
      ASSERT_EQ(list.size(), 3u);
      for (size_t n = 0; n < list.size(); ++n) EXPECT_EQ(list[n], neighbors[n]);
      EXPECT_THROW(view.vertex(1), std::out_of_range);
      EXPECT_THROW(list[3], std::out_of_range);
    }
    serialized[40] = std::byte{0xff}; // Bad offset is checked on vertex access, not header load.
    multiscale_csr_page_view_t view(serialized);
    EXPECT_THROW(view.neighbors(0), std::invalid_argument);
    serialized[16] = std::byte{0xff};
    EXPECT_THROW((multiscale_csr_page_view_t(serialized)), std::invalid_argument);
  }
}

TEST_P(MultiscaleBufferTest, DisabledOverflowAndGuardOutlivesWrapper) {
  multiscale_disk_manager_t disk(directory, disk_options);
  const auto id = disk.allocate_page(page_space_t::CSR_4K);
  options.groups[0] = {0, 0};
  { multiscale_buf_pool_t pool(disk, options);
    EXPECT_THROW((void)pool.fetch_async(id).result.get(), std::runtime_error);
    EXPECT_THROW((void)pool.fetch_async(page_id_t{}).result.get(), std::invalid_argument);
    EXPECT_EQ(pool.stats(id.space()).chunk_bytes, 0u); }
  options.groups[0] = {std::numeric_limits<size_t>::max(), 2};
  EXPECT_THROW((multiscale_buf_pool_t(disk, options)), std::overflow_error);
  options.groups[0] = {1, 1};
  page_pin_t pin;
  { multiscale_buf_pool_t pool(disk, options); pin = pool.fetch_async(id).result.get(); }
  { auto guard = std::move(pin).read(); EXPECT_EQ(guard.bytes()[0], std::byte{0}); }
}
TEST_P(MultiscaleBufferTest, ConcurrentDirtyEvictionAndFlush) {
  options.groups[0] = {2, 4};
  multiscale_disk_manager_t disk(directory, disk_options);
  std::array<page_id_t, 16> ids;
  for (auto& id : ids) id = disk.allocate_page(page_space_t::CSR_4K);
  multiscale_buf_pool_t pool(disk, options);
  std::atomic<bool> failed{false};
  std::vector<std::thread> writers;
  for (size_t t = 0; t < 4; ++t) writers.emplace_back([&, t] {
    try {
      for (size_t j = 0; j < 200; ++j) {
        auto guard = pool.fetch_write(ids[(j * 7 + t) % ids.size()]);
        uint64_t value;
        std::memcpy(&value, guard.bytes().data(), 8);
        ++value;
        std::memcpy(guard.bytes().data(), &value, 8);
      }
    } catch (...) { failed = true; }
  });
  // Concurrent flush is safe but not a durability boundary until writers stop.
  std::thread flusher([&] {
    try { for (size_t j = 0; j < 10; ++j) pool.flush_all(); }
    catch (...) { failed = true; }
  });
  for (auto& writer : writers) writer.join();
  flusher.join();
  EXPECT_FALSE(failed);
  ASSERT_TRUE(pool.close());
  uint64_t total = 0;
  std::array<uint64_t, 16> expected{};
  for (size_t t = 0; t < 4; ++t)
    for (size_t j = 0; j < 200; ++j) ++expected[(j * 7 + t) % ids.size()];
  for (auto id : ids) {
    auto data = disk.read_page(id);
    uint64_t count;
    std::memcpy(&count, data.bytes().data(), 8);
    EXPECT_EQ(count, expected[id.slot()]);
    total += count;
  }
  EXPECT_EQ(total, 800u);
  EXPECT_GT(pool.stats(page_space_t::CSR_4K).evictions, 0u);
}

TEST_P(MultiscaleBufferTest, MixedSpaceBatchAndMovedGuards) {
  multiscale_disk_manager_t disk(directory, disk_options);
  multiscale_buf_pool_t pool(disk, options);
  std::array<page_id_t, 6> ids;
  for (size_t i = 0; i < 6; ++i) ids[i] = disk.allocate_page(static_cast<page_space_t>(i));
  auto requests = pool.fetch_batch(ids);
  for (auto& request : requests) {
    auto first = request.result.get().write();
    auto second = std::move(first);
    second.bytes()[0] = std::byte{17};
    EXPECT_THROW(first.bytes(), std::logic_error);
  }
  EXPECT_TRUE(pool.close());
  for (auto id : ids) EXPECT_EQ(disk.read_page(id).bytes()[0], std::byte{17});
}

TEST_P(MultiscaleBufferTest, ConcurrentLookupCannotPinReusedWrongPage) {
  options.groups[0] = {4, 2};
  multiscale_disk_manager_t disk(directory, disk_options);
  std::vector<page_id_t> ids;
  for (size_t i = 0; i < 128; ++i) {
    const auto id = disk.allocate_page(page_space_t::CSR_4K);
    auto data = disk.read_page(id);
    std::fill(data.bytes().begin(), data.bytes().end(), std::byte(i));
    disk.write_page(data);
    ids.push_back(id);
  }
  multiscale_buf_pool_t pool(disk, options);
  std::atomic<bool> failed{false};
  std::vector<std::thread> readers;
  for (size_t t = 0; t < 4; ++t) readers.emplace_back([&, t] {
    try {
      for (size_t j = 0; j < 1000; ++j) {
        const auto index = (j * 17 + t * 31) % ids.size();
        auto guard = pool.fetch_read(ids[index]);
        if (guard.page_id() != ids[index] || guard.bytes().front() != std::byte(index) ||
            guard.bytes().back() != std::byte(index)) failed = true;
      }
    } catch (...) { failed = true; }
  });
  for (auto& reader : readers) reader.join();
  EXPECT_FALSE(failed);
  EXPECT_TRUE(pool.close());
  EXPECT_GT(pool.stats(page_space_t::CSR_4K).evictions, 100u);
}

TEST_P(MultiscaleBufferTest, ChunkPinsOutliveAllPoolMetadata) {
  multiscale_disk_manager_t disk(directory, disk_options);
  std::vector<page_pin_t> pins;
  {
    multiscale_buf_pool_t pool(disk, options);
    for (size_t c = 0; c < 6; ++c) {
      auto id = disk.allocate_page(static_cast<page_space_t>(c));
      pins.push_back(pool.fetch_async(id).result.get());
    }
    EXPECT_FALSE(pool.close());
  }
  for (auto& pin : pins) {
    auto guard = std::move(pin).read();
    EXPECT_EQ(guard.bytes().size(), disk.page_size(guard.page_id().space()));
    EXPECT_EQ(guard.bytes().back(), std::byte{0});
  }
}

TEST_P(MultiscaleBufferTest, ResidentPrefetchDoesNotPinOrAllocateRequest) {
  multiscale_disk_manager_t disk(directory, disk_options);
  const auto id = disk.allocate_page(page_space_t::CSR_4K);
  multiscale_buf_pool_t pool(disk, options);
  { auto guard = pool.fetch_read(id); }
  for (size_t i = 0; i < 100; ++i) pool.prefetch(id);
  EXPECT_EQ(pool.stats(id.space()).hits, 0u);
  EXPECT_EQ(pool.stats(id.space()).disk_reads, 1u);
  EXPECT_EQ(pool.stats(id.space()).pinned_frames, 0u);
  EXPECT_TRUE(pool.close());
  EXPECT_NO_THROW(pool.prefetch(id));
}

INSTANTIATE_TEST_SUITE_P(CacheModes, MultiscaleBufferTest, ::testing::Bool());
} // namespace

TEST_P(MultiscaleBufferTest, StaleFrameHintCannotReadReusedPage) {
  options.groups[0] = {1, 1};
  multiscale_disk_manager_t disk(directory, disk_options);
  const auto first = disk.allocate_pages(page_space_t::CSR_4K, 3);
  std::array<page_frame_hint_t, 3> hints;
  multiscale_buf_pool_t pool(disk, options);
  for (uint32_t i = 0; i < 3; ++i) {
    const auto id = page_id_t::make(first.space(), first.slot() + i);
    auto guard = pool.fetch_write(id);
    guard.bytes()[0] = std::byte(i + 17);
  }
  for (size_t round = 0; round < 20; ++round)
    for (uint32_t i = 0; i < 3; ++i) {
      const auto id = page_id_t::make(first.space(), first.slot() + i);
      auto guard = pool.fetch_read(id, hints[i]);
      EXPECT_EQ(guard.page_id(), id);
      EXPECT_EQ(guard.bytes()[0], std::byte(i + 17));
    }
  EXPECT_TRUE(pool.close());
}

namespace {
TEST_P(MultiscaleBufferTest, FreshFramesSkipReadsAndPersist) {
  multiscale_disk_manager_t disk(directory, disk_options);
  multiscale_buf_pool_t pool(disk, options);
  auto id = disk.allocate_page(page_space_t::CSR_32K);
  {
    auto guard = pool.new_page(id);
    EXPECT_TRUE(std::all_of(guard.bytes().begin(), guard.bytes().end(),
                           [](auto b) { return b == std::byte{0}; }));
    guard.bytes().front() = std::byte{42}; guard.bytes().back() = std::byte{19};
  }
  EXPECT_EQ(pool.stats(id.space()).disk_reads, 0u);
  EXPECT_THROW((void)pool.new_page(id), std::logic_error);
  pool.flush_all();
  const auto page = disk.read_page(id);
  EXPECT_EQ(page.bytes().front(), std::byte{42});
  EXPECT_EQ(page.bytes().back(), std::byte{19});
}
}

TEST_P(MultiscaleBufferTest, StaleWriteHintRevalidatesIdentityAndPersistsEachPage) {
  options.groups[5] = {1, 1};
  multiscale_disk_manager_t disk(directory, disk_options);
  const auto first = disk.allocate_pages(page_space_t::DELTA, 3);
  std::array<page_frame_hint_t, 3> hints;
  multiscale_buf_pool_t pool(disk, options);
  for (uint32_t round = 0; round < 20; ++round) {
    for (uint32_t i = 0; i < 3; ++i) {
      const auto id = page_id_t::make(first.space(), first.slot() + i);
      auto guard = pool.fetch_write(id, hints[i]);
      EXPECT_EQ(guard.page_id(), id);
      if (round) {
        EXPECT_EQ(guard.bytes()[0], std::byte((round - 1) * 3 + i));
      }
      guard.bytes()[0] = std::byte(round * 3 + i);
    }
  }
  EXPECT_TRUE(pool.close());
  for (uint32_t i = 0; i < 3; ++i) {
    const auto page = disk.read_page(page_id_t::make(first.space(), first.slot() + i));
    EXPECT_EQ(page.bytes()[0], std::byte(19 * 3 + i));
  }
}
