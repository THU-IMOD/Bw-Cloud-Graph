#ifndef BW_GRAPH_COMMON_MULTISCALE_BUFFER_CONFIG_H
#define BW_GRAPH_COMMON_MULTISCALE_BUFFER_CONFIG_H

#include <array>
#include <cstddef>

namespace bw_graph {
struct buffer_group_config_t {
  size_t chunk_count{0};
  size_t chunk_size{0}; // Frames per chunk, NOT bytes.
};
struct multiscale_buf_options_t {
  // CSR 4K/32K/256K/2M, Index, Delta. Defaults: 44 MiB of frame data with a 16K Index.
  std::array<buffer_group_config_t, 6> groups{{
      {2, 1024}, {2, 128}, {2, 16}, {2, 2}, {1, 256}, {2, 1024}}};
};
} // namespace bw_graph
#endif
