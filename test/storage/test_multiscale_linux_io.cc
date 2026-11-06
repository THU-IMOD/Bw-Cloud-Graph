#ifdef __linux__
#include "bw_graph/storage/multiscale_disk_manager.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <gtest/gtest.h>
#include <system_error>
#include <unistd.h>

namespace {
// GNU ld wrappers inject failures only into this test executable.
std::atomic<int> read_interrupts{0}, write_interrupts{0}, sync_error{0};
std::atomic<bool> short_transfers{false}, fail_write{false};
std::atomic<void*> observed_destination{nullptr};
}
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" int __real_fdatasync(int);
extern "C" ssize_t __wrap_pread(int fd, void* data, size_t size, off_t offset) {
  observed_destination.store(data);
  if (read_interrupts.exchange(0)) { errno = EINTR; return -1; }
  return __real_pread(fd, data, short_transfers ? std::min(size, size_t{4096}) : size, offset);
}
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, size_t size, off_t offset) {
  if (write_interrupts.exchange(0)) { errno = EINTR; return -1; }
  if (fail_write && size <= 4096) { errno = EIO; return -1; }
  return __real_pwrite(fd, data, short_transfers ? std::min(size, size_t{4096}) : size, offset);
}
extern "C" int __wrap_fdatasync(int fd) {
  if (const int error = sync_error.exchange(0)) { errno = error; return -1; }
  return __real_fdatasync(fd);
}

using namespace bw_graph;
namespace {
class LinuxPageIO : public ::testing::Test {
protected:
  void SetUp() override {
    std::string pattern = (std::filesystem::temp_directory_path() / "bw-io-fault-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    directory = pattern;
  }
  void TearDown() override {
    short_transfers = false;
    fail_write = false;
    read_interrupts = write_interrupts = sync_error = 0;
    std::filesystem::remove_all(directory);
  }
  std::filesystem::path directory;
};

TEST_F(LinuxPageIO, DirectAlignedShortTransfersAndInterruptedSyscalls) {
  multiscale_disk_manager_t disk(directory);
  const auto id = disk.allocate_page(page_space_t::CSR_32K);
  disk_page_t expected(id, disk.page_size(id.space()));
  std::fill(expected.bytes().begin(), expected.bytes().end(), std::byte{0x7b});
  short_transfers = true;
  write_interrupts = 1;
  disk.write_page(expected);
  read_interrupts = 1;
  const auto actual = disk.read_page(id);
  EXPECT_TRUE(std::equal(expected.bytes().begin(), expected.bytes().end(), actual.bytes().begin()));
  sync_error = EINTR;
  EXPECT_NO_THROW(disk.flush());
}

TEST_F(LinuxPageIO, PartialWriteAndSyncFailureRemainDirty) {
  multiscale_disk_manager_t disk(directory);
  const auto id = disk.allocate_page(page_space_t::CSR_32K);
  disk_page_t page(id, disk.page_size(id.space()));
  std::fill(page.bytes().begin(), page.bytes().end(), std::byte{0x4c});
  disk.flush();
  const auto before = disk.get_sync_count(id.space());
  short_transfers = true;
  fail_write = true;
  EXPECT_THROW(disk.write_page(page), std::system_error);
  fail_write = false;
  sync_error = EIO;
  EXPECT_THROW(disk.flush(), std::system_error);
  EXPECT_EQ(disk.get_sync_count(id.space()), before);
  disk.flush();
  EXPECT_EQ(disk.get_sync_count(id.space()), before + 1);
  disk.write_page(page);
  disk.shutdown();
  multiscale_disk_manager_t reopened(directory);
  const auto actual = reopened.read_page(id);
  EXPECT_TRUE(std::equal(page.bytes().begin(), page.bytes().end(), actual.bytes().begin()));
}

TEST_F(LinuxPageIO, AsyncUsesCallerAddressAndDrainsBeforeDestruction) {
  std::future<disk_page_t> result;
  const auto id = page_id_t::make(page_space_t::CSR_2M, 0);
  disk_page_t frame(id, 2097152);
  {
    multiscale_disk_manager_t disk(directory);
    ASSERT_EQ(disk.allocate_page(id.space()), id);
    std::fill(frame.bytes().begin(), frame.bytes().end(), std::byte{0x2a});
    disk.write_page(frame);
    std::fill(frame.bytes().begin(), frame.bytes().end(), std::byte{0});
    std::promise<void> done;
    auto finished = done.get_future();
    disk.read_into_async(id, frame.bytes(), [&](std::exception_ptr error) {
      if (error) done.set_exception(error);
      else done.set_value();
    });
    finished.get();
    EXPECT_EQ(observed_destination.load(), frame.bytes().data());
    EXPECT_TRUE(std::all_of(frame.bytes().begin(), frame.bytes().end(),
                           [](std::byte b) { return b == std::byte{0x2a}; }));
    result = disk.read_page_async(id).result;
  }
  const auto actual = result.get();
  EXPECT_TRUE(std::equal(frame.bytes().begin(), frame.bytes().end(), actual.bytes().begin()));
}
} // namespace
#endif
