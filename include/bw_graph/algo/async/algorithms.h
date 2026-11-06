#pragma once
#include "bw_graph/algo/byo.h"

namespace bw_graph::async_algo {
// Independent implementation of ACGraph's page worklist / monotone relaxation model.
// Static native base graph only. Scheduler metadata contains IDs, never edge payloads.
byo::bfs_result_t bfs(const byo::graph_t& graph, v_id_t source,
                     options_t options, stats_t* stats = nullptr);
std::vector<v_id_t> connected_components(const byo::graph_t& graph,
                                        options_t options, stats_t* stats = nullptr);
// BYO borrowed page scans + ACGraph-like immediate monotone propagation inside a page.
// Page bitmap deduplicates work; bounded local closure replaces the shared priority queue.
byo::bfs_result_t page_bfs(const byo::graph_t& graph, v_id_t source,
                          options_t options, stats_t* stats = nullptr);
}
