#include "bw_graph/common/config.h"
#include "bw_graph/common/type.h"
#include "bw_graph/db/db.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sys/types.h>
#include <utility>

int main(int argc, char* argv[]) {
  std::string config_path = "config/default.yaml";

  if (argc > 1) {
    config_path = argv[1];
    std::cout << "Config path: " << config_path << std::endl;
    load_yaml_config(config_path);
  } else {
    std::cout << "Use Default Configuration." << std::endl;
    load_yaml_config();
  }

  print_config();

  std::string g_name = bw_graph::GRAPH_NAME;

  // Load input graph file
  std::string test_file = "data/" + g_name + ".graph";

  std::cout << "Creating database.." << std::endl;
  auto db =
      bw_graph_db_t(g_name, bw_graph::BW_GRAPH_REBUILD_INDEX, bw_graph::BW_GRAPH_REDO_PARTITION,
                    false, bw_graph::BW_GRAPH_ID_AWARE, true, true);
  std::cout << "Database created successfully" << std::endl;
  db.warm_up();

  auto start_time = std::chrono::high_resolution_clock::now();
  std::vector<std::pair<v_id_t, uint64_t>> res = db.bfs(1);
  auto end_time = std::chrono::high_resolution_clock::now();

  auto duration_us =
      std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

  std::cout << "BFS execution time: " << format_duration(duration_us) << " (" << duration_us
            << " μs)" << std::endl;

  assert(db.graph_name_ == g_name);

  std::cout << "BFS vertex count: " << res.size() << " from v1;" << std::endl;
  return 0;
}