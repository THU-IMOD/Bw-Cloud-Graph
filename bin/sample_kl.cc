#include "bw_graph/common/config.h"
#include "bw_graph/common/distribution.h"
#include "bw_graph/mem/mem_g0com.h"
#include "bw_graph/mem/mem_g0map.h"

#include <cassert>
#include <iostream>
#include <memory>
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

  // Warm up the database
  // Load the graph.
  std::string g_name = bw_graph::GRAPH_NAME;
  std::cout << "Creating Memory Graph" << std::endl;
  std::string graph_file = "data/" + g_name + ".graph";
  mem_sub_com_t com_graph(graph_file, true, bw_graph::LOAD_BUFFER_SIZE);

  // Graph sampling - Page method
  std::cout << "Starting random walk sampling..." << std::endl;

  std::shared_ptr<mem_sub_map_t> sampled_graph_random_walk =
      com_graph.graph_sample_random_walk(0, bw_graph::BW_GRAPH_SAMPLE_VERTEX_COUNT);

  std::cout << "Sampled graph (random walk) vertices: " << sampled_graph_random_walk->vertex_count()
            << ", edges: " << sampled_graph_random_walk->edge_count() << std::endl;

  // Graph sampling - BFS method
  std::cout << "Starting BFS sampling..." << std::endl;

  std::shared_ptr<mem_sub_map_t> sampled_graph_bfs =
      com_graph.graph_sample_bfs(0, bw_graph::BW_GRAPH_SAMPLE_VERTEX_COUNT);

  std::cout << "Sampled graph (BFS) vertices: " << sampled_graph_bfs->vertex_count()
            << ", edges: " << sampled_graph_bfs->edge_count() << std::endl;

  // Compute the degree distribution - Page
  std::cout << "Computing degree distribution (random walk)..." << std::endl;

  auto distribution_random_walk = sampled_graph_random_walk->compute_degree_distribution(
      bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS);

  // Compute the degree distribution - Walk
  std::cout << "Computing degree distribution (BFS)..." << std::endl;

  auto distribution_bfs =
      sampled_graph_bfs->compute_degree_distribution(bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS);

  // Compute the degree distribution - Ground truth
  std::cout << "Computing degree distribution (ground truth)..." << std::endl;

  auto distribution_ground_truth =
      com_graph.compute_degree_distribution(bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS);

  // Compute KL divergence between distributions
  std::cout << "Computing KL divergence..." << std::endl;

  double kl_random_walk = compute_symmetric_kl_divergence(
      distribution_ground_truth, distribution_random_walk,
      bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS, bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS);
  double kl_bfs = compute_symmetric_kl_divergence(distribution_ground_truth, distribution_bfs,
                                                  bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS,
                                                  bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS);

  // Print results
  std::cout << "========================================" << std::endl;
  std::cout << "Graph Sampling Performance Results" << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << "Random Walk Sampling:" << std::endl;
  std::cout << "  Vertices: " << sampled_graph_random_walk->vertex_count() << std::endl;
  std::cout << "  KL Divergence: " << kl_random_walk << std::endl;
  std::cout << "----------------------------------------" << std::endl;
  std::cout << "BFS Sampling:" << std::endl;
  std::cout << "  Vertices: " << sampled_graph_bfs->vertex_count() << std::endl;
  std::cout << "  Edges: " << sampled_graph_bfs->edge_count() << std::endl;
  std::cout << "  KL Divergence: " << kl_bfs << std::endl;
  std::cout << "----------------------------------------" << std::endl;
  std::cout << "========================================" << std::endl;

  return 0;
}