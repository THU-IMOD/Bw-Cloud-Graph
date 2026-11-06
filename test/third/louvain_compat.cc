#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include "louvain.hxx"

TEST(LouvainCompatibility, WeightedDisconnectedCommunities) {
  const int previous_threads = omp_get_max_threads();
  for (int threads : {1, 2}) {
    SCOPED_TRACE(threads);
    omp_set_num_threads(threads);
    DiGraph<uint32_t, None, float> graph;
    graph.reserve(7, 2);
    for (uint32_t v = 1; v <= 6; ++v) graph.addVertex(v);
    for (uint32_t base : {1U, 4U}) {
      for (uint32_t u = base; u < base + 3; ++u) {
        for (uint32_t v = u + 1; v < base + 3; ++v) {
          const float weight = static_cast<float>(v - u + 1);
          graph.addEdge(u, v, weight);
          graph.addEdge(v, u, weight);
        }
      }
    }
    graph.update();
    const auto result = louvainStaticOmp(graph, LouvainOptions(1));
    EXPECT_EQ(result.membership.size(), 7U);
    if (result.membership.size() != 7U) continue;
    EXPECT_EQ(result.membership[1], result.membership[2]);
    EXPECT_EQ(result.membership[2], result.membership[3]);
    EXPECT_EQ(result.membership[4], result.membership[5]);
    EXPECT_EQ(result.membership[5], result.membership[6]);
    EXPECT_NE(result.membership[1], result.membership[4]);
    auto community = [&](auto v) { return result.membership[v]; };
    EXPECT_NEAR(modularityBy(graph, community, edgeWeightOmp(graph) / 2, 1.0), 0.5, 1e-6);
  }
  omp_set_num_threads(previous_threads);
}
