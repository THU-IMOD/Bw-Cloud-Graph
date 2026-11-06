#pragma once
#include "bw_graph/common/vertex_index_backend.h"
#include <string>
// Test/benchmark-only option. Consume it before each harness parses positional args.
inline bw_graph::vertex_index_backend_t vertex_backend_option(int& argc, char** argv) {
  auto backend = bw_graph::BW_VERTEX_INDEX_BACKEND;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) != "--vertex-index") continue;
    if (i + 1 >= argc) throw std::invalid_argument("--vertex-index requires array or art");
    backend = bw_graph::parse_vertex_index_backend(argv[i + 1]);
    for (int j = i; j + 2 <= argc; ++j) argv[j] = argv[j + 2];
    argc -= 2;
    --i;
  }
  bw_graph::BW_VERTEX_INDEX_BACKEND = backend;
  return backend;
}
