#include "bw_graph/storage/multiscale_disk_manager.h"
#include "bw_graph/storage/s3_store.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <set>
#include <unistd.h>

using namespace bw_graph;

namespace {
constexpr std::array<page_space_t, 6> all_spaces{
    page_space_t::CSR_4K, page_space_t::CSR_32K, page_space_t::CSR_256K,
    page_space_t::CSR_2M, page_space_t::INDEX, page_space_t::DELTA};

class MultiscaleDiskTest : public ::testing::TestWithParam<bool> {
protected:
  void SetUp() override {
    std::string name = (std::filesystem::temp_directory_path() /
                        "bw-multiscale-disk-XXXXXX").string();
    const char* created = ::mkdtemp(name.data());
    ASSERT_NE(created, nullptr);
    directory = created;
    options.bypass_cache = GetParam();
    options.io_workers = 3;
    options.queue_capacity = 2; // Force backpressure on batches larger than the queue.
  }
  void TearDown() override { std::filesystem::remove_all(directory); }
  std::filesystem::path directory;
  multiscale_disk_options_t options;
};

TEST(PageId, EncodingAndBounds) {
  EXPECT_FALSE(page_id_t{}.valid());
  for (auto space : all_spaces) {
    for (uint32_t slot : {0u, 17u, page_id_t::slot_mask}) {
      auto id = page_id_t::make(space, slot);
      EXPECT_EQ(id.space(), space);
      EXPECT_EQ(id.slot(), slot);
      EXPECT_EQ(page_id_t::from_raw(id.raw()), id);
    }
  }
  EXPECT_EQ(page_id_t::make(page_space_t::CSR_4K, 0).raw(), 0u);
  EXPECT_THROW(page_id_t{}.space(), std::invalid_argument);
  EXPECT_THROW(page_id_t::from_raw(UINT32_MAX), std::invalid_argument);
  EXPECT_THROW(page_id_t::from_raw(6u << 29), std::invalid_argument);
  EXPECT_THROW(page_id_t::make(page_space_t::INDEX, 1u << 29), std::out_of_range);
  EXPECT_THROW(page_id_t::make(static_cast<page_space_t>(9), 0), std::out_of_range);
}

TEST_P(MultiscaleDiskTest, SixSpacesReopenOverwriteAndZeroAllocation) {
  const std::array<uint32_t, 6> expected_sizes{
      4096, 32768, 262144, 2097152, options.index_page_size, options.delta_page_size};
  const std::array<const char*, 6> expected_files{
      "csr_4k.pages", "csr_32k.pages", "csr_256k.pages", "csr_2m.pages", "index.pages",
      "delta.pages"};
  {
    multiscale_disk_manager_t disk(directory, options);
    for (size_t i = 0; i < all_spaces.size(); ++i) {
      const auto space = all_spaces[i];
      EXPECT_EQ(disk.page_size(space), expected_sizes[i]);
      auto first = disk.allocate_page(space);
      EXPECT_EQ(first.slot(), 0u);
      auto zero = disk.read_page(first);
      EXPECT_TRUE(std::all_of(zero.bytes().begin(), zero.bytes().end(),
                              [](std::byte b) { return b == std::byte{0}; }));
      EXPECT_EQ(reinterpret_cast<uintptr_t>(zero.bytes().data()) % 4096, 0u);
      std::fill(zero.bytes().begin(), zero.bytes().end(), std::byte(i + 1));
      disk.write_page(zero);
      auto second = disk.allocate_page(space);
      auto page = disk.read_page(second);
      std::fill(page.bytes().begin(), page.bytes().end(), std::byte(i + 21));
      disk.write_page(page);
      // Overwrite the first slot without damaging the second or another space.
      std::fill(zero.bytes().begin(), zero.bytes().end(), std::byte(i + 11));
      disk.write_page(zero);
      disk.allocate_page(space); // Trailing all-zero allocated page must survive restart.
    }
    disk.flush();
    disk.shutdown();
    if (s3_enabled()) EXPECT_TRUE(std::filesystem::is_empty(directory));
    else for (size_t i = 0; i < expected_files.size(); ++i) {
      EXPECT_EQ(std::filesystem::file_size(directory / expected_files[i]),
                multiscale_disk_manager_t::file_header_bytes + uint64_t{3} * expected_sizes[i]);
    }
    EXPECT_NO_THROW(disk.shutdown());
    EXPECT_THROW(disk.read_page(page_id_t::make(page_space_t::CSR_4K, 0)), std::runtime_error);
    EXPECT_THROW(disk.read_page_async(page_id_t::make(page_space_t::CSR_4K, 0)),
                 std::runtime_error);
    EXPECT_THROW(disk.allocate_page(page_space_t::CSR_4K), std::runtime_error);
  }
  multiscale_disk_manager_t reopened(directory, options);
  for (size_t i = 0; i < all_spaces.size(); ++i) {
    EXPECT_EQ(reopened.get_page_count(all_spaces[i]), 3u);
    for (uint32_t slot = 0; slot < 3; ++slot) {
      const auto page = reopened.read_page(page_id_t::make(all_spaces[i], slot));
      const auto expected = slot == 0 ? std::byte(i + 11) :
                            slot == 1 ? std::byte(i + 21) : std::byte{0};
      EXPECT_TRUE(std::all_of(page.bytes().begin(), page.bytes().end(),
                              [expected](std::byte b) { return b == expected; }));
    }
    EXPECT_EQ(reopened.allocate_page(all_spaces[i]).slot(), 3u);
  }
}

TEST_P(MultiscaleDiskTest, MixedBatchAsyncOrderErrorsAndShutdown) {
  std::vector<page_read_request_t> pending;
  std::vector<page_id_t> ids;
  {
    multiscale_disk_manager_t disk(directory, options);
    for (auto space : all_spaces) {
      auto id = disk.allocate_page(space);
      auto page = disk.read_page(id);
      std::fill(page.bytes().begin(), page.bytes().end(), std::byte(static_cast<unsigned>(space)));
      disk.write_page(page);
      ids.push_back(id);
    }
    std::reverse(ids.begin(), ids.end());
    ids.push_back(ids.front());
    auto sync = disk.read_pages(ids);
    ASSERT_EQ(sync.size(), ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
      EXPECT_EQ(sync[i].page_id(), ids[i]);
      EXPECT_EQ(sync[i].bytes().front(), std::byte(static_cast<unsigned>(ids[i].space())));
    }
    EXPECT_TRUE(disk.read_pages({}).empty());
    EXPECT_TRUE(disk.read_pages_async({}).empty());
    auto single = disk.read_page_async(ids[0]);
    EXPECT_EQ(single.result.get().page_id(), ids[0]);
    const auto missing = page_id_t::make(page_space_t::CSR_4K, 100);
    auto failure = disk.read_page_async(missing);
    EXPECT_THROW((void)failure.result.get(), std::out_of_range);
    std::array<page_id_t, 3> bad_batch{ids[0], missing, ids[1]};
    auto mixed = disk.read_pages_async(bad_batch);
    EXPECT_EQ(mixed[0].result.get().page_id(), ids[0]);
    EXPECT_THROW((void)mixed[1].result.get(), std::out_of_range);
    EXPECT_EQ(mixed[2].result.get().page_id(), ids[1]);
    EXPECT_THROW(disk.read_pages(bad_batch), std::out_of_range);
    pending = disk.read_pages_async(ids);
  } // Destructor must drain reads before closing fds. Futures outlive the manager.
  ASSERT_EQ(pending.size(), ids.size());
  for (size_t i = 0; i < ids.size(); ++i) {
    auto page = pending[i].result.get();
    EXPECT_EQ(page.page_id(), pending[i].page_id);
    EXPECT_TRUE(std::all_of(page.bytes().begin(), page.bytes().end(), [&](std::byte b) {
      return b == std::byte(static_cast<unsigned>(ids[i].space()));
    }));
    auto moved = std::move(page);
    EXPECT_TRUE(page.bytes().empty());
    EXPECT_EQ(moved.page_id(), ids[i]);
  }
}

TEST_P(MultiscaleDiskTest, ConcurrentAllocationAndSubmission) {
  multiscale_disk_manager_t disk(directory, options);
  std::vector<std::future<std::vector<uint32_t>>> jobs;
  for (unsigned worker = 0; worker < 4; ++worker) {
    jobs.push_back(std::async(std::launch::async, [&disk, worker] {
      std::vector<uint32_t> slots;
      for (unsigned n = 0; n < 12; ++n) {
        auto id = disk.allocate_page(page_space_t::CSR_4K);
        disk_page_t page(id, 4096);
        std::fill(page.bytes().begin(), page.bytes().end(), std::byte(worker + 1));
        disk.write_page(page);
        auto read = disk.read_page_async(id).result.get();
        if (!std::equal(page.bytes().begin(), page.bytes().end(), read.bytes().begin())) {
          throw std::runtime_error("Concurrent data mismatch");
        }
        slots.push_back(id.slot());
      }
      return slots;
    }));
  }
  std::set<uint32_t> slots;
  for (auto& job : jobs) for (auto slot : job.get()) EXPECT_TRUE(slots.insert(slot).second);
  EXPECT_EQ(slots.size(), 48u);
  EXPECT_EQ(disk.get_page_count(page_space_t::CSR_4K), 48u);
}

TEST_P(MultiscaleDiskTest, RejectInvalidInputAndTruncation) {
  multiscale_disk_manager_t disk(directory, options);
  auto id = disk.allocate_page(page_space_t::CSR_4K);
  std::vector<std::byte> buffer(4097, std::byte{42});
  // Explicitly unaligned span exercises the O_DIRECT bounce path on Linux.
  auto unaligned = std::span(buffer).subspan(1);
  disk.write_page(id, unaligned);
  std::fill(unaligned.begin(), unaligned.end(), std::byte{0});
  disk.read_page(id, unaligned);
  EXPECT_TRUE(std::all_of(unaligned.begin(), unaligned.end(),
                          [](std::byte b) { return b == std::byte{42}; }));
  EXPECT_THROW(disk.write_page(id, buffer), std::invalid_argument);
  EXPECT_THROW(disk.read_page(id, std::span(buffer).first(1)), std::invalid_argument);
  EXPECT_THROW(disk.read_page(page_id_t{}), std::invalid_argument);
  EXPECT_THROW(disk.read_pages_async(std::array<page_id_t, 1>{page_id_t{}}),
               std::invalid_argument);
  EXPECT_THROW(disk.write_page(page_id_t::make(page_space_t::CSR_4K, 1), unaligned),
               std::out_of_range);
  EXPECT_THROW((void)multiscale_disk_manager_t(directory, options), std::system_error);
  if (s3_enabled()) {
    s3_store_t objects; std::vector<std::byte> manifest;
    const auto prefix = "pages/" + s3_path_key(directory);
    ASSERT_TRUE(objects.get(prefix + "/manifest", manifest));
    auto doc = nlohmann::json::parse(reinterpret_cast<char*>(manifest.data()), reinterpret_cast<char*>(manifest.data()+manifest.size()));
    objects.put(prefix + '/' + doc["generation"].get<std::string>() + "/0/0", {});
  } else ASSERT_EQ(::truncate((directory / "csr_4k.pages").c_str(), 4096), 0);
  EXPECT_THROW(disk.read_page(id), std::runtime_error);
  auto request = disk.read_page_async(id);
  EXPECT_THROW((void)request.result.get(), std::runtime_error);
}

TEST_P(MultiscaleDiskTest, ShutdownWakesBatchProducer) {
  auto opts = options;
  opts.io_workers = 1;
  opts.queue_capacity = 1;
  multiscale_disk_manager_t disk(directory, opts);
  const auto id = disk.allocate_page(page_space_t::CSR_4K);
  std::vector<page_id_t> ids(512, id);
  std::promise<void> started;
  auto ready = started.get_future();
  auto producer = std::async(std::launch::async, [&] {
    started.set_value();
    std::vector<page_read_request_t> requests;
    try {
      requests = disk.read_pages_async(ids);
    } catch (const std::runtime_error&) {
      // Shutdown may reject a batch before or during bounded-queue submission.
      return;
    }
    for (auto& request : requests) (void)request.result.get();
  });
  ready.get();
  disk.shutdown();
  EXPECT_EQ(producer.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_NO_THROW(producer.get());
}

TEST_P(MultiscaleDiskTest, RejectHeaderSizeAndPartialFile) {
  {
    auto custom = options;
    custom.index_page_size = 32768;
    multiscale_disk_manager_t disk(directory, custom);
    disk.allocate_page(page_space_t::INDEX);
  }
  EXPECT_THROW((void)multiscale_disk_manager_t(directory, options), std::runtime_error);
  auto custom = options;
  custom.index_page_size = 32768;
  // Constructor failure must release earlier fds/locks so a correct reopen works.
  { multiscale_disk_manager_t reopened(directory, custom); }
  if (s3_enabled()) {
    s3_store_t objects; std::vector<std::byte> manifest;
    const auto key = "pages/" + s3_path_key(directory) + "/manifest";
    ASSERT_TRUE(objects.get(key, manifest));
    auto doc = nlohmann::json::parse(reinterpret_cast<char*>(manifest.data()), reinterpret_cast<char*>(manifest.data()+manifest.size()));
    auto corrupt = [&](const nlohmann::json& value) {
      auto text = value.dump(); objects.put(key, std::as_bytes(std::span(text.data(), text.size())));
    };
    auto invalid = doc; invalid["format"] = "unknown"; corrupt(invalid);
    EXPECT_THROW((void)multiscale_disk_manager_t(directory, custom), std::runtime_error);
    invalid = doc; invalid["counts"][0] = uint64_t(page_id_t::slot_mask) + 2; corrupt(invalid);
    EXPECT_THROW((void)multiscale_disk_manager_t(directory, custom), std::runtime_error);
    corrupt(doc);
    { multiscale_disk_manager_t reopened(directory, custom); }
  } else {
  {
    std::fstream file(directory / "csr_4k.pages", std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file);
    file.seekp(8);
    file.put('\1'); // Previous store version, whose middle CSR spaces were 64/512 KiB.
  }
  EXPECT_THROW((void)multiscale_disk_manager_t(directory, custom), std::runtime_error);
  {
    std::fstream file(directory / "csr_4k.pages", std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file);
    file.seekp(8);
    file.put('\2');
  }
  {
    std::fstream file(directory / "delta.pages", std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file);
    file.put('X');
  }
  EXPECT_THROW((void)multiscale_disk_manager_t(directory, custom), std::runtime_error);
  { // Restore magic and append one byte: reject incomplete slot rather than rounding down.
    std::fstream file(directory / "delta.pages", std::ios::binary | std::ios::in | std::ios::out);
    file.put('B');
    file.seekp(0, std::ios::end);
    file.put('x');
  }
  EXPECT_THROW((void)multiscale_disk_manager_t(directory, custom), std::runtime_error);
  }
  custom.io_workers = 0;
  EXPECT_THROW((void)multiscale_disk_manager_t(directory, custom), std::invalid_argument);
  custom.io_workers = 1;
  custom.queue_capacity = 0;
  EXPECT_THROW((void)multiscale_disk_manager_t(directory, custom), std::invalid_argument);
  custom.queue_capacity = 1;
  custom.delta_page_size = 4097;
  EXPECT_THROW((void)multiscale_disk_manager_t(directory, custom), std::invalid_argument);
}

TEST_P(MultiscaleDiskTest, AsyncBorrowedDestinationValidationAndErrors) {
  multiscale_disk_manager_t disk(directory, options);
  const auto id = disk.allocate_page(page_space_t::CSR_32K);
  disk_page_t source(id, disk.page_size(id.space()));
  std::fill(source.bytes().begin(), source.bytes().end(), std::byte{37});
  disk.write_page(source);
  disk_page_t destination(id, disk.page_size(id.space()));
  auto* address = destination.bytes().data();
  std::promise<void> done;
  disk.read_into_async(id, destination.bytes(), [&](std::exception_ptr error) {
    if (error) done.set_exception(error); else done.set_value();
  });
  done.get_future().get();
  EXPECT_EQ(destination.bytes().data(), address);
  EXPECT_TRUE(std::equal(source.bytes().begin(), source.bytes().end(), destination.bytes().begin()));
  std::atomic<size_t> callbacks{0};
  EXPECT_THROW(disk.read_into_async(id, destination.bytes().subspan(1),
      [&](auto) { ++callbacks; }), std::invalid_argument);
  EXPECT_THROW(disk.read_into_async(id, destination.bytes(), {}), std::invalid_argument);
  std::promise<void> failed;
  disk.read_into_async(page_id_t::make(id.space(), 999), destination.bytes(), [&](auto error) {
    ++callbacks;
    if (error) failed.set_exception(error); else failed.set_value();
  });
  EXPECT_THROW(failed.get_future().get(), std::out_of_range);
  disk.shutdown();
  EXPECT_EQ(callbacks.load(), 1u);
}

TEST_P(MultiscaleDiskTest, FlushOnlyChangedSpacesAndConcurrentWrites) {
  multiscale_disk_manager_t disk(directory, options);
  disk.flush();
  for (auto space : all_spaces) EXPECT_EQ(disk.get_sync_count(space), 1u);
  disk.flush();
  for (auto space : all_spaces) EXPECT_EQ(disk.get_sync_count(space), 1u);
  const auto id = disk.allocate_page(page_space_t::CSR_4K);
  disk.flush();
  EXPECT_EQ(disk.get_sync_count(id.space()), 2u); // Allocation changes file length.
  EXPECT_EQ(disk.get_sync_count(page_space_t::CSR_2M), 1u);
  auto page = disk.read_page(id);
  disk.flush();
  EXPECT_EQ(disk.get_sync_count(id.space()), 2u); // A read does not dirty a file.
  std::thread writer([&] {
    for (size_t i = 1; i <= 100; ++i) {
      std::fill(page.bytes().begin(), page.bytes().end(), std::byte(i));
      disk.write_page(page);
    }
  });
  std::thread syncer([&] { for (size_t i = 0; i < 20; ++i) disk.flush(); });
  writer.join(); syncer.join();
  disk.flush(); // Quiescent durability boundary must include the final write.
  const auto count = disk.get_sync_count(id.space());
  EXPECT_GT(count, 2u);
  EXPECT_EQ(disk.get_sync_count(page_space_t::CSR_2M), 1u);
  disk.flush();
  EXPECT_EQ(disk.get_sync_count(id.space()), count);
  disk.shutdown();
  multiscale_disk_manager_t reopened(directory, options);
  const auto bytes = reopened.read_page(id);
  EXPECT_TRUE(std::all_of(bytes.bytes().begin(), bytes.bytes().end(),
                         [](auto value) { return value == std::byte{100}; }));
}

INSTANTIATE_TEST_SUITE_P(CacheModes, MultiscaleDiskTest, ::testing::Bool());
} // namespace
