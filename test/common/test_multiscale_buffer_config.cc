#include "bw_graph/common/config.h"
#include <gtest/gtest.h>
#include <fstream>
#include <unistd.h>

TEST(MultiscaleBufferConfig, DefaultsPartialAndExpressions) {
  auto config = parse_multiscale_buffer_config(YAML::Node(YAML::NodeType::Undefined));
  EXPECT_EQ(config.groups[0].chunk_size, 1024u);
  config = parse_multiscale_buffer_config(YAML::Load(
      "csr_4k: {chunk_count: 3, chunk_size: 2 * 8}\n"
      "csr_2m: {chunk_count: 0, chunk_size: 0}\n"));
  EXPECT_EQ(config.groups[0].chunk_count, 3u);
  EXPECT_EQ(config.groups[0].chunk_size, 16u);
  EXPECT_EQ(config.groups[1].chunk_size, 128u);
  EXPECT_EQ(config.groups[3].chunk_count, 0u);
}
TEST(MultiscaleBufferConfig, RejectsMalformedAndOverflowingCounts) {
  for (const auto* text : {"-1", "1.5", "4oops", "1 *", "*2", "2**3", ""}) {
    EXPECT_THROW(parse_buffer_count(YAML::Node(text)), std::invalid_argument) << text;
  }
  for (const auto* text : {"[]", "null"}) {
    EXPECT_THROW(parse_buffer_count(YAML::Load(text)), std::invalid_argument) << text;
  }
  for (const auto* text : {"18446744073709551616", "18446744073709551615 * 2"}) {
    EXPECT_THROW(parse_buffer_count(YAML::Node(text)), std::overflow_error) << text;
  }
  for (const auto* text : {"[]", "csr_64k: {}", "csr_4k: []", "index: {pages: 2}",
      "csr_4k: {chunk_count: 1, chunk_size: 0}"}) {
    EXPECT_THROW(parse_multiscale_buffer_config(YAML::Load(text)), std::invalid_argument) << text;
  }
  EXPECT_THROW(parse_multiscale_buffer_config(YAML::Load(
      "csr_4k: {chunk_count: 18446744073709551615, chunk_size: 2}")), std::overflow_error);
}
TEST(MultiscaleBufferConfig, LoaderCommitsOnlyValidNewBudgetsAndResetsMissingDefaults) {
  struct restore_t {
    bw_graph::multiscale_buf_options_t old = bw_graph::BW_MULTISCALE_BUFFER_CONFIG;
    bool engine = bw_graph::BW_MULTISCALE_ENGINE;
    bool warm = bw_graph::BW_MULTISCALE_FULL_GIANT_WARMUP;
    ~restore_t() {
      bw_graph::BW_MULTISCALE_BUFFER_CONFIG = old;
      bw_graph::BW_MULTISCALE_ENGINE = engine;
      bw_graph::BW_MULTISCALE_FULL_GIANT_WARMUP = warm;
    }
  } restore;
  char path[] = "/tmp/bw-buffer-config-XXXXXX";
  const int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);
  struct cleanup_t { const char* p; ~cleanup_t() { unlink(p); } } cleanup{path};
  auto write = [&](const char* text) { std::ofstream out(path); out << text; };
  write("multiscale_buffer: {csr_4k: {chunk_count: 7, chunk_size: 9}}\n");
  ASSERT_NO_THROW(load_yaml_config(path));
  EXPECT_EQ(bw_graph::BW_MULTISCALE_BUFFER_CONFIG.groups[0].chunk_count, 7u);
  write("multiscale_buffer: {csr_4k: {chunk_count: -1}}\n");
  EXPECT_THROW(load_yaml_config(path), std::invalid_argument);
  EXPECT_EQ(bw_graph::BW_MULTISCALE_BUFFER_CONFIG.groups[0].chunk_count, 7u);
  write("storage: {engine: multiscale, full_giant_warmup: false}\n");
  ASSERT_NO_THROW(load_yaml_config(path));
  EXPECT_TRUE(bw_graph::BW_MULTISCALE_ENGINE);
  EXPECT_FALSE(bw_graph::BW_MULTISCALE_FULL_GIANT_WARMUP);
  write("storage: {engine: invalid}\n");
  EXPECT_THROW(load_yaml_config(path), std::invalid_argument);
  EXPECT_TRUE(bw_graph::BW_MULTISCALE_ENGINE);
  write("{}\n");
  ASSERT_NO_THROW(load_yaml_config(path));
  EXPECT_EQ(bw_graph::BW_MULTISCALE_BUFFER_CONFIG.groups[0].chunk_count, 2u);
  EXPECT_FALSE(bw_graph::BW_MULTISCALE_ENGINE);
  EXPECT_TRUE(bw_graph::BW_MULTISCALE_FULL_GIANT_WARMUP);
}
