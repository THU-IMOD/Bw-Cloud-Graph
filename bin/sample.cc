#include "bw_graph/common/config.h"
#include "bw_graph/common/distribution.h"
#include "bw_graph/db/db.h"
#include "bw_graph/mem/mem_g0map.h"

#include <cassert>
#include <chrono>
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

  std::string g_name = bw_graph::GRAPH_NAME;

  std::cout << "Creating database.." << std::endl;
  auto db =
      bw_graph_db_t(g_name, bw_graph::BW_GRAPH_REBUILD_INDEX, bw_graph::BW_GRAPH_REDO_PARTITION);
  assert(db.graph_name_ == g_name);
  std::cout << "Database created successfully" << std::endl;

  // Warm up the database
  db.warm_up();

  // Graph sampling - Page method
  std::cout << "Starting page sampling..." << std::endl;
  auto start_page = std::chrono::high_resolution_clock::now();

  std::shared_ptr<mem_sub_map_t> sampled_graph_page = db.graph_sample_page(0);

  auto end_page = std::chrono::high_resolution_clock::now();
  auto duration_page = std::chrono::duration_cast<std::chrono::microseconds>(end_page - start_page);
  double page_sampling_time_us = duration_page.count();

  std::cout << "Sampled graph (page) vertices: " << sampled_graph_page->vertex_count()
            << ", edges: " << sampled_graph_page->edge_count() << std::endl;

  // Graph sampling - Walk method
  std::cout << "Starting walk sampling..." << std::endl;
  auto start_walk = std::chrono::high_resolution_clock::now();

  std::shared_ptr<mem_sub_map_t> sampled_graph_walk = db.graph_sample_walk(0, 200);

  auto end_walk = std::chrono::high_resolution_clock::now();
  auto duration_walk = std::chrono::duration_cast<std::chrono::microseconds>(end_walk - start_walk);
  double walk_sampling_time_us = duration_walk.count();

  std::cout << "Sampled graph (walk) vertices: " << sampled_graph_walk->vertex_count()
            << ", edges: " << sampled_graph_walk->edge_count() << std::endl;

  // Compute the degree distribution - Page
  std::cout << "Computing degree distribution (page)..." << std::endl;
  auto start_dist_page = std::chrono::high_resolution_clock::now();

  auto distribution_page =
      sampled_graph_page->compute_degree_distribution(bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS);

  auto end_dist_page = std::chrono::high_resolution_clock::now();
  auto duration_dist_page =
      std::chrono::duration_cast<std::chrono::microseconds>(end_dist_page - start_dist_page);
  double dist_page_time_us = duration_dist_page.count();

  // Compute the degree distribution - Walk
  std::cout << "Computing degree distribution (walk)..." << std::endl;
  auto start_dist_walk = std::chrono::high_resolution_clock::now();

  auto distribution_walk =
      sampled_graph_walk->compute_degree_distribution(bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS);

  auto end_dist_walk = std::chrono::high_resolution_clock::now();
  auto duration_dist_walk =
      std::chrono::duration_cast<std::chrono::microseconds>(end_dist_walk - start_dist_walk);
  double dist_walk_time_us = duration_dist_walk.count();

  // Compute the degree distribution - Ground truth
  std::cout << "Computing degree distribution (ground truth)..." << std::endl;
  auto start_dist_gt = std::chrono::high_resolution_clock::now();

  auto distribution_ground_truth =
      db.compute_degree_distribution(bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS);

  auto end_dist_gt = std::chrono::high_resolution_clock::now();
  auto duration_dist_gt =
      std::chrono::duration_cast<std::chrono::microseconds>(end_dist_gt - start_dist_gt);
  double dist_gt_time_us = duration_dist_gt.count();

  // Compute KL divergence between distributions
  std::cout << "Computing KL divergence..." << std::endl;
  auto start_kl = std::chrono::high_resolution_clock::now();

  double kl_page = compute_symmetric_kl_divergence(distribution_ground_truth, distribution_page,
                                                   bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS,
                                                   bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS);
  double kl_walk = compute_symmetric_kl_divergence(distribution_ground_truth, distribution_walk,
                                                   bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS,
                                                   bw_graph::BW_GRAPH_SAMPLE_DISTRIBUTION_STEPS);

  auto end_kl = std::chrono::high_resolution_clock::now();
  auto duration_kl = std::chrono::duration_cast<std::chrono::microseconds>(end_kl - start_kl);
  double kl_time_us = duration_kl.count();

  // Print results
  std::cout << "========================================" << std::endl;
  std::cout << "Graph Sampling Performance Results" << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << "Page Sampling:" << std::endl;
  std::cout << "  Vertices: " << sampled_graph_page->vertex_count() << std::endl;
  std::cout << "  Edges: " << sampled_graph_page->edge_count() << std::endl;
  std::cout << "  Sampling time: " << page_sampling_time_us << " us" << std::endl;
  std::cout << "  Distribution computation time: " << dist_page_time_us << " us" << std::endl;
  std::cout << "  KL Divergence: " << kl_page << std::endl;
  std::cout << "----------------------------------------" << std::endl;
  std::cout << "Walk Sampling:" << std::endl;
  std::cout << "  Vertices: " << sampled_graph_walk->vertex_count() << std::endl;
  std::cout << "  Edges: " << sampled_graph_walk->edge_count() << std::endl;
  std::cout << "  Sampling time: " << walk_sampling_time_us << " us" << std::endl;
  std::cout << "  Distribution computation time: " << dist_walk_time_us << " us" << std::endl;
  std::cout << "  KL Divergence: " << kl_walk << std::endl;
  std::cout << "----------------------------------------" << std::endl;
  std::cout << "Ground Truth:" << std::endl;
  std::cout << "  Distribution computation time: " << dist_gt_time_us << " us" << std::endl;
  std::cout << "----------------------------------------" << std::endl;
  std::cout << "KL Divergence computation time: " << kl_time_us << " us" << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << "Summary:" << std::endl;
  std::cout << "  Page sampling is "
            << (page_sampling_time_us < walk_sampling_time_us ? "faster" : "slower")
            << " than walk sampling" << std::endl;
  std::cout << "  Time ratio (page/walk): " << page_sampling_time_us / walk_sampling_time_us << "x"
            << std::endl;
  std::cout << "========================================" << std::endl;

  return 0;
}