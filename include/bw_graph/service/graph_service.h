#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bw_graph::service {
// Transport-neutral API. Handles are registry tokens, never native addresses.
// All IDs and values are opaque bytes. Each named graph is an independent store.
class graph_service_t {
public:
  graph_service_t();
  ~graph_service_t();
  uint64_t open(const std::filesystem::path& root, const std::string& name);
  bool newly_created(uint64_t graph);
  void close(uint64_t graph);
  void reset(const std::filesystem::path& root, const std::string& name);
  uint64_t create_vertex(uint64_t graph, const std::string& id, const std::string& data);
  uint64_t create_edge(uint64_t graph, const std::string& id, uint64_t src, uint64_t dst,
                       const std::string& data);
  int64_t find(uint64_t graph, bool edge, const std::string& id);
  std::string external_id(uint64_t graph, bool edge, uint64_t id);
  std::string get(uint64_t graph, bool edge, uint64_t id);
  void put(uint64_t graph, bool edge, uint64_t id, const std::string& data);
  void remove(uint64_t graph, bool edge, uint64_t id);
  std::vector<uint64_t> all(uint64_t graph, bool edges);
  std::vector<uint64_t> incident(uint64_t graph, uint64_t vertex, int direction);
  std::vector<uint64_t> bfs(uint64_t graph, uint64_t source);
  std::vector<uint64_t> components(uint64_t graph, bool strong);
  std::vector<uint64_t> communities(uint64_t graph);

private:
  struct impl_t;
  std::unique_ptr<impl_t> impl_;
};
} // namespace bw_graph::service
