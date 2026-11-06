#include "bw_graph/common/vertex_index_backend.h"
#include <gtest/gtest.h>
#include <string>
namespace {
struct backend_listener_t : ::testing::EmptyTestEventListener {
  bw_graph::vertex_index_backend_t backend;
  explicit backend_listener_t(bw_graph::vertex_index_backend_t b) : backend(b) {}
  void OnTestStart(const ::testing::TestInfo&) override {
    bw_graph::BW_VERTEX_INDEX_BACKEND = backend;
  }
};
}
int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--art") {
    bw_graph::BW_VERTEX_INDEX_BACKEND = bw_graph::vertex_index_backend_t::art;
    for (int i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc;
  }
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::UnitTest::GetInstance()->listeners().Append(
      new backend_listener_t(bw_graph::BW_VERTEX_INDEX_BACKEND));
  return RUN_ALL_TESTS();
}
