#pragma once
#include <optional>
#include <stdexcept>
#include <string_view>

namespace bw_graph {
enum class graph_layout_t { id_based, kaminpar_gorder, louvain_gorder };
inline std::optional<graph_layout_t> BW_GRAPH_LAYOUT;
inline graph_layout_t parse_graph_layout(std::string_view name) {
  if (name == "id-based" || name == "id") return graph_layout_t::id_based;
  if (name == "kaminpar-gorder") return graph_layout_t::kaminpar_gorder;
  if (name == "louvain-gorder") return graph_layout_t::louvain_gorder;
  throw std::invalid_argument(
      "partition.layout must be id-based, kaminpar-gorder, or louvain-gorder");
}
inline const char* graph_layout_name(graph_layout_t layout) {
  switch (layout) {
    case graph_layout_t::id_based: return "id-based";
    case graph_layout_t::kaminpar_gorder: return "kaminpar-gorder";
    case graph_layout_t::louvain_gorder: return "louvain-gorder";
  }
  throw std::invalid_argument("Invalid graph layout");
}
} // namespace bw_graph
