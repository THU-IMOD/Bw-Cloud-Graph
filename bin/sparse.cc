#include "bw_graph/common/type.h"
#include "bw_graph/mem/mem_g0com.h"
#include "bw_graph/part/partition.h"
#include "test.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <nlohmann/json.hpp>

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
  std::cout << "\n========== Loading Graph ==========\n";
  std::cout << "Loading graph from: " << test_file << std::endl;

  mem_sub_com_t com_graph(test_file, true);
  adj_map_t graph = com_graph.compute_induced_graph_map();

  // Print original graph statistics
  std::cout << "\n========== Original Graph Statistics ==========\n";
  std::cout << "Graph name: " << g_name << std::endl;
  std::cout << "Number of nodes: " << graph.size() << std::endl;

  // Calculate original graph metrics
  size_t total_edges = 0;
  size_t min_degree = SIZE_MAX;
  size_t max_degree = 0;
  size_t total_degree = 0;

  for (const auto& [node, neighbors] : graph) {
    size_t degree = neighbors.size();
    total_edges += degree;
    total_degree += degree;
    min_degree = std::min(min_degree, degree);
    max_degree = std::max(max_degree, degree);
  }

  total_edges /= 2; // Each edge counted twice
  double avg_degree = graph.empty() ? 0.0 : (double) total_degree / graph.size();

  std::cout << "Total edges: " << total_edges << std::endl;
  std::cout << "Average degree: " << std::fixed << std::setprecision(2) << avg_degree << std::endl;
  std::cout << "Min degree: " << (min_degree == SIZE_MAX ? 0 : min_degree) << std::endl;
  std::cout << "Max degree: " << max_degree << std::endl;
  std::cout << "Graph density: " << std::fixed << std::setprecision(4)
            << (graph.size() > 1 ? (2.0 * total_edges) / (graph.size() * (graph.size() - 1)) : 0.0)
            << std::endl;

  // Check original graph connectivity
  std::cout << "\n========== Original Graph Connectivity ==========\n";
  bool original_connected = is_connected(graph);
  std::cout << "Original graph is " << (original_connected ? "CONNECTED" : "DISCONNECTED")
            << std::endl;

  if (!original_connected) {
    // Count connected components
    std::set<page_no_t> visited;
    int components = 0;
    std::vector<size_t> component_sizes;

    for (const auto& [start_node, _] : graph) {
      if (visited.find(start_node) == visited.end()) {
        components++;
        size_t component_size = 0;
        std::queue<page_no_t> queue;
        queue.push(start_node);
        visited.insert(start_node);

        while (!queue.empty()) {
          page_no_t current = queue.front();
          queue.pop();
          component_size++;

          auto it = graph.find(current);
          if (it != graph.end()) {
            for (page_no_t neighbor : it->second) {
              if (visited.find(neighbor) == visited.end()) {
                visited.insert(neighbor);
                queue.push(neighbor);
              }
            }
          }
        }
        component_sizes.push_back(component_size);
      }
    }

    std::cout << "Number of connected components: " << components << std::endl;
    std::cout << "Component sizes: ";
    for (size_t i = 0; i < component_sizes.size(); ++i) {
      std::cout << component_sizes[i];
      if (i < component_sizes.size() - 1)
        std::cout << ", ";
    }
    std::cout << std::endl;

    // Find largest component
    auto max_it = std::max_element(component_sizes.begin(), component_sizes.end());
    if (max_it != component_sizes.end()) {
      std::cout << "Largest component size: " << *max_it << " (" << std::fixed
                << std::setprecision(1) << (100.0 * (*max_it) / graph.size()) << "% of total nodes)"
                << std::endl;
    }
  }

  // Perform graph sparsification
  std::cout << "\n========== Graph Sparsification ==========\n";
  uint64_t max_degree_limit = 3;
  std::cout << "Applying sparsification with max_degree = " << max_degree_limit << std::endl;

  auto start_time = std::chrono::high_resolution_clock::now();
  auto result = graph_sparsification(graph, max_degree_limit);
  auto end_time = std::chrono::high_resolution_clock::now();

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
  std::cout << "Sparsification completed in " << duration.count() << " ms" << std::endl;

  // Print sparsified graph statistics
  std::cout << "\n========== Sparsified Graph Statistics ==========\n";
  std::cout << "Number of nodes: " << result.size() << std::endl;

  size_t sparsified_edges = 0;
  size_t sparsified_min_degree = SIZE_MAX;
  size_t sparsified_max_degree = 0;
  size_t sparsified_total_degree = 0;

  for (const auto& [node, neighbors] : result) {
    size_t degree = neighbors.size();
    sparsified_edges += degree;
    sparsified_total_degree += degree;
    sparsified_min_degree = std::min(sparsified_min_degree, degree);
    sparsified_max_degree = std::max(sparsified_max_degree, degree);
  }

  sparsified_edges /= 2;
  double sparsified_avg_degree =
      result.empty() ? 0.0 : (double) sparsified_total_degree / result.size();

  std::cout << "Total edges: " << sparsified_edges << std::endl;
  std::cout << "Average degree: " << std::fixed << std::setprecision(2) << sparsified_avg_degree
            << std::endl;
  std::cout << "Min degree: " << (sparsified_min_degree == SIZE_MAX ? 0 : sparsified_min_degree)
            << std::endl;
  std::cout << "Max degree: " << sparsified_max_degree << std::endl;
  std::cout << "Graph density: " << std::fixed << std::setprecision(4)
            << (result.size() > 1 ? (2.0 * sparsified_edges) / (result.size() * (result.size() - 1))
                                  : 0.0)
            << std::endl;

  // Sparsification efficiency metrics
  std::cout << "\n========== Sparsification Efficiency ==========\n";
  double edge_reduction =
      total_edges > 0 ? (double) (total_edges - sparsified_edges) / total_edges * 100 : 0.0;
  double degree_reduction =
      avg_degree > 0 ? (avg_degree - sparsified_avg_degree) / avg_degree * 100 : 0.0;

  std::cout << "Edge reduction: " << std::fixed << std::setprecision(1) << edge_reduction << "%"
            << std::endl;
  std::cout << "Average degree reduction: " << std::fixed << std::setprecision(1)
            << degree_reduction << "%" << std::endl;
  std::cout << "Compression ratio: " << std::fixed << std::setprecision(2)
            << (total_edges > 0 ? (double) sparsified_edges / total_edges : 0.0) << std::endl;

  // Check degree constraints
  std::cout << "\n========== Degree Constraint Verification ==========\n";
  bool constraints_satisfied = check_degree_constraints(result, max_degree_limit);
  std::cout << "Degree constraints " << (constraints_satisfied ? "SATISFIED" : "VIOLATED")
            << std::endl;

  if (!constraints_satisfied) {
    std::cout << "ERROR: Some nodes exceed the maximum degree limit!" << std::endl;
    // Print violating nodes
    for (const auto& [node, neighbors] : result) {
      if (neighbors.size() > max_degree_limit) {
        std::cout << "  Node " << node << " has degree " << neighbors.size() << " (exceeds limit "
                  << max_degree_limit << ")" << std::endl;
      }
    }
  } else {
    std::cout << "All nodes respect the maximum degree constraint of " << max_degree_limit
              << std::endl;
  }

  // Check sparsified graph connectivity
  std::cout << "\n========== Sparsified Graph Connectivity ==========\n";
  bool sparsified_connected = is_connected(result);
  std::cout << "Sparsified graph is " << (sparsified_connected ? "CONNECTED" : "DISCONNECTED")
            << std::endl;

  if (!sparsified_connected && !result.empty()) {
    // Count connected components in sparsified graph
    std::set<page_no_t> visited;
    int components = 0;
    std::vector<size_t> component_sizes;

    for (const auto& [start_node, _] : result) {
      if (visited.find(start_node) == visited.end()) {
        components++;
        size_t component_size = 0;
        std::queue<page_no_t> queue;
        queue.push(start_node);
        visited.insert(start_node);

        while (!queue.empty()) {
          page_no_t current = queue.front();
          queue.pop();
          component_size++;

          auto it = result.find(current);
          if (it != result.end()) {
            for (page_no_t neighbor : it->second) {
              if (visited.find(neighbor) == visited.end()) {
                visited.insert(neighbor);
                queue.push(neighbor);
              }
            }
          }
        }
        component_sizes.push_back(component_size);
      }
    }

    std::cout << "Number of connected components: " << components << std::endl;
    std::cout << "Component sizes: ";
    uint64_t printed_components = 0;
    for (size_t i = 0; i < component_sizes.size(); ++i) {
      if (printed_components > 10) {
        std::cout << "...(total " << component_sizes.size() << " components)" << std::endl;
        break;
      }
      std::cout << component_sizes[i];
      if (i < component_sizes.size() - 1)
        std::cout << ", ";
      printed_components++;
    }
    std::cout << std::endl;

    // Find largest component
    auto max_it = std::max_element(component_sizes.begin(), component_sizes.end());
    if (max_it != component_sizes.end()) {
      std::cout << "Largest component size: " << *max_it << " (" << std::fixed
                << std::setprecision(1) << (100.0 * (*max_it) / result.size())
                << "% of sparsified nodes)" << std::endl;
    }
  }

  // Connectivity preservation analysis
  std::cout << "\n========== Connectivity Preservation Analysis ==========\n";
  if (original_connected && sparsified_connected) {
    std::cout << "✓ Connectivity preserved successfully" << std::endl;
  } else if (original_connected && !sparsified_connected) {
    std::cout << "✗ Connectivity lost during sparsification" << std::endl;
  } else if (!original_connected && sparsified_connected) {
    std::cout << "◎ Connectivity improved (original was disconnected)" << std::endl;
  } else {
    std::cout << "◦ Both graphs are disconnected" << std::endl;
  }

  // Summary
  std::cout << "\n========== Summary ==========\n";
  std::cout << "Graph: " << g_name << std::endl;
  std::cout << "Original: " << graph.size() << " nodes, " << total_edges << " edges" << std::endl;
  std::cout << "Sparsified: " << result.size() << " nodes, " << sparsified_edges << " edges"
            << std::endl;
  std::cout << "Max degree limit: " << max_degree_limit << " (constraint "
            << (constraints_satisfied ? "satisfied" : "violated") << ")" << std::endl;
  std::cout << "Edge reduction: " << std::fixed << std::setprecision(1) << edge_reduction << "%"
            << std::endl;
  std::cout << "Connectivity: " << (original_connected ? "preserved" : "original was disconnected")
            << std::endl;
  std::cout << "Processing time: " << duration.count() << " ms" << std::endl;

  return 0;
}