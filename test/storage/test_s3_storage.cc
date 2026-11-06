#include "bw_graph/storage/s3_store.h"
#include "bw_graph/storage/multiscale_disk_manager.h"
#include "bw_graph/buf/multiscale_buf_pool.h"
#include "bw_graph/service/graph_service.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <cerrno>
#include <system_error>
#include <unistd.h>
using namespace bw_graph;
namespace {
class S3Storage : public ::testing::Test {
protected:
  std::filesystem::path path;
  void SetUp() override {
    if (!s3_enabled()) GTEST_SKIP() << "Requires BWGRAPH_STORAGE_BACKEND=s3 and a dedicated S3_PREFIX";
    char pattern[] = "/tmp/bw-s3-regression-XXXXXX"; path = mkdtemp(pattern);
  }
  void TearDown() override { if (!path.empty()) std::filesystem::remove_all(path); }
  std::string page_key(page_id_t id) {
    s3_store_t objects; std::vector<std::byte> bytes;
    const auto prefix = "pages/" + s3_path_key(path);
    if (!objects.get(prefix + "/manifest", bytes)) throw std::runtime_error("Manifest absent");
    auto doc = nlohmann::json::parse(reinterpret_cast<char*>(bytes.data()), reinterpret_cast<char*>(bytes.data() + bytes.size()));
    return prefix + '/' + doc["generation"].get<std::string>() + '/' +
           std::to_string(static_cast<unsigned>(id.space())) + '/' + std::to_string(id.slot());
  }
};
TEST_F(S3Storage, SixSpacesZeroPagesAsyncAndReopenWithoutLocalFiles) {
  std::vector<page_id_t> ids;
  {
    multiscale_disk_manager_t disk(path);
    try {
      multiscale_disk_manager_t duplicate(path);
      FAIL() << "A second writer acquired the same S3 store";
    } catch (const std::system_error& error) {
      EXPECT_EQ(error.code(), std::error_code(EBUSY, std::generic_category()));
    } catch (const std::exception& error) {
      FAIL() << "Unexpected duplicate-open exception: " << error.what();
    }
    for (unsigned i = 0; i < 6; ++i) {
      auto space = static_cast<page_space_t>(i); auto id = disk.allocate_pages(space, 2); ids.push_back(id);
      auto zero = disk.read_page(id);
      EXPECT_TRUE(std::all_of(zero.bytes().begin(), zero.bytes().end(), [](auto x) { return x == std::byte{0}; }));
      std::fill(zero.bytes().begin(), zero.bytes().end(), std::byte(i + 1)); disk.write_page(zero);
      disk_page_t frame(id, disk.page_size(space)); std::promise<void> done; auto future = done.get_future();
      disk.read_into_async(id, frame.bytes(), [&](auto error) { if (error) done.set_exception(error); else done.set_value(); });
      future.get(); EXPECT_EQ(frame.bytes().back(), std::byte(i + 1));
    }
    auto requests = disk.read_pages_async(ids); disk.shutdown();
    for (size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(requests[i].result.get().bytes().front(), std::byte(i + 1));
  }
  EXPECT_TRUE(std::filesystem::is_empty(path)); // No local page or edge cache.
  std::filesystem::remove_all(path);
  multiscale_disk_manager_t reopened(path);
  for (size_t i = 0; i < ids.size(); ++i) {
    EXPECT_EQ(reopened.get_page_count(ids[i].space()), 2u);
    EXPECT_EQ(reopened.read_page(ids[i]).bytes().back(), std::byte(i + 1));
    EXPECT_EQ(reopened.read_page(page_id_t::make(ids[i].space(), 1)).bytes().back(), std::byte{0});
  }
}
TEST_F(S3Storage, MissingTruncatedObjectsAndBufferFailureRecovery) {
  multiscale_disk_manager_t disk(path);
  auto id = disk.allocate_page(page_space_t::CSR_4K); auto key = page_key(id);
  s3_store_t objects; objects.erase(key);
  multiscale_buf_options_t options; for (auto& g : options.groups) g = {1, 1};
  multiscale_buf_pool_t pool(disk, options);
  EXPECT_THROW((void)pool.fetch_async(id).result.get(), std::runtime_error);
  EXPECT_EQ(pool.stats(id.space()).pinned_frames, 0u);
  objects.put(key, std::vector<std::byte>(3));
  EXPECT_THROW((void)disk.read_page_async(id).result.get(), std::runtime_error);
  disk_page_t page(id, 4096); std::fill(page.bytes().begin(), page.bytes().end(), std::byte{7}); disk.write_page(page);
  { auto guard = pool.fetch_read(id); EXPECT_EQ(guard.bytes().back(), std::byte{7}); }
  pool.close(); disk.shutdown();
}
TEST_F(S3Storage, RejectsNarrowingManifestValuesAndReleasesOwnership) {
  { multiscale_disk_manager_t disk(path); disk.allocate_page(page_space_t::CSR_4K); }
  s3_store_t objects;
  std::vector<std::byte> bytes;
  const auto key = "pages/" + s3_path_key(path) + "/manifest";
  ASSERT_TRUE(objects.get(key, bytes));
  const auto original = nlohmann::json::parse(
      reinterpret_cast<const char*>(bytes.data()),
      reinterpret_cast<const char*>(bytes.data() + bytes.size()));
  for (const nlohmann::json& value :
       {nlohmann::json(uint64_t{1} << 32), nlohmann::json(-1), nlohmann::json(1.5)}) {
    auto invalid = original;
    invalid["counts"][0] = value;
    const auto text = invalid.dump();
    objects.put(key, std::as_bytes(std::span(text.data(), text.size())));
    EXPECT_THROW((void)multiscale_disk_manager_t(path), std::runtime_error);
    objects.put(key, bytes);
    // Every rejected constructor must release its remote owner token.
    multiscale_disk_manager_t reopened(path);
    EXPECT_EQ(reopened.get_page_count(page_space_t::CSR_4K), 1u);
  }
}
TEST_F(S3Storage, MetadataPropertiesResetAndReopenWithoutLocalDirectory) {
  service::graph_service_t service;
  auto h = service.open(path, "graph");
  auto a = service.create_vertex(h, "a", "alice"), b = service.create_vertex(h, "b", "bob");
  auto edge = service.create_edge(h, "ab", a, b, "edge-property"); service.close(h);
  std::filesystem::remove_all(path / "graph");
  h = service.open(path, "graph");
  EXPECT_EQ(service.find(h, false, "a"), a); EXPECT_EQ(service.get(h, false, a), "alice");
  EXPECT_EQ(service.get(h, true, edge), "edge-property");
  EXPECT_EQ(service.incident(h, b, 1), std::vector<uint64_t>{edge});
  EXPECT_THROW(service.reset(path, "graph"), std::runtime_error);
  service.close(h); service.reset(path, "graph");
  std::filesystem::remove_all(path / "graph");
  h = service.open(path, "graph"); EXPECT_TRUE(service.all(h, false).empty()); service.close(h);
}
}
