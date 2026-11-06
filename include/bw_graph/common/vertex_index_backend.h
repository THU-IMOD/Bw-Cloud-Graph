#ifndef BW_GRAPH_COMMON_VERTEX_INDEX_BACKEND_H
#define BW_GRAPH_COMMON_VERTEX_INDEX_BACKEND_H
#include <stdexcept>
#include <string>
namespace bw_graph {
enum class vertex_index_backend_t { array, art };
inline vertex_index_backend_t BW_VERTEX_INDEX_BACKEND = vertex_index_backend_t::array;
inline vertex_index_backend_t parse_vertex_index_backend(const std::string& name) {
  if (name == "array") return vertex_index_backend_t::array;
  if (name == "art") return vertex_index_backend_t::art;
  throw std::invalid_argument("index.vertex_index must be array or art");
}
inline const char* vertex_index_backend_name(vertex_index_backend_t backend) {
  return backend == vertex_index_backend_t::art ? "art" : "array";
}
}
#endif
