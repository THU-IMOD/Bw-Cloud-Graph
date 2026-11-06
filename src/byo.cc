// Adapted from BYO/GBBS (MIT), commit 00f8ead3925a3346d4322fd2b6f0b1ba1f1f9834.
// Copyright (c) 2018 Laxman Dhulipala, Guy Blelloch, and Julian Shun.
// Full permission and warranty notice: include/bw_graph/algo/BYO_LICENSE.txt.
#include "bw_graph/algo/byo.h"
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <tbb/parallel_reduce.h>
#include <bit>
#include <cmath>
#include <numeric>
#include <random>

namespace bw_graph::byo {
namespace {
constexpr auto none = UINT32_MAX;
template <class T> std::atomic_ref<T> atom(T& x) { return std::atomic_ref<T>(x); }
struct frontier_t {
  tbb::enumerable_thread_specific<std::vector<v_id_t>> pieces;
  void clear() { for (auto& p : pieces) p.clear(); }
  void add(v_id_t v) { pieces.local().push_back(v); }
  void collect(std::vector<v_id_t>& out) {
    size_t n = 0;
    for (auto& p : pieces) n += p.size();
    out.resize(n);
    size_t offset = 0;
    for (auto& p : pieces) {
      std::copy(p.begin(), p.end(), out.begin() + offset);
      offset += p.size();
    }
  }
};
uint64_t hash(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
template <class T, class F> T sum(size_t n, T identity, F&& f) {
  return tbb::parallel_reduce(tbb::blocked_range<size_t>(0, n, 2048), identity,
      [&](const auto& r, T value) {
        for (size_t i = r.begin(); i < r.end(); ++i) value += f(i);
        return value;
      }, std::plus<T>());
}
void source_check(const graph_t& g, v_id_t source) {
  if (source >= g.n()) throw std::out_of_range("BYO source vertex");
}
}
graph_t::graph_t(bw_graph_db_t& db, bool page_aware, bool undirected)
    : db_(db), page_aware_(page_aware && !undirected), undirected_(undirected),
      base_fits_(!db.multiscale || db.multiscale->base_fits()), degrees_(db.get_vertex_count()) {
  auto fill_degree = [&](size_t v) {
    uint64_t degree = 0;
    if (undirected_) vertex(v, [&](auto, auto&& range) { degree = range.size(); });
    else degree = db.multiscale ? db.multiscale->base_degree(v) : db.read_neighbor(v).size();
    if (degree > UINT32_MAX) throw std::overflow_error("BYO degree exceeds 32-bit vertex domain");
    degrees_[v] = degree;
  };
  if (undirected_ && !fits()) directional_for(n(), fill_degree);
  else tbb::parallel_for(size_t(0), n(), fill_degree);
  for (size_t v = 0; v < n(); ++v) {
    edges_ += degrees_[v];
    if (!degrees_[v]) isolates_.push_back(v);
  }
}

bfs_result_t bfs(const graph_t& g, v_id_t source, size_t divisor) {
  source_check(g, source);
  if (!divisor) throw std::invalid_argument("BFS divisor must be positive");
  bfs_result_t result{std::vector<v_id_t>(g.n(), none), std::vector<uint32_t>(g.n(), none)};
  result.parent[source] = source;
  result.distance[source] = 0;
  std::vector<v_id_t> frontier{source}, next;
  std::vector<uint8_t> bitmap;
  frontier_t output;
  uint32_t level = 0;
  while (!frontier.empty()) {
    output.clear();
    const auto work = sum<uint64_t>(frontier.size(), 0,
        [&](size_t i) { return g.degree(frontier[i]); });
    // BYO edgeMap accounts for active edges, unlike the old vertex-count-only switch.
    const bool dense = work + frontier.size() > g.m() / divisor && frontier.size() > 64;
    if (dense) {
      bitmap.assign(g.n(), 0);
      for (auto v : frontier) bitmap[v] = 1;
      g.incoming_if([&](v_id_t v) { return result.parent[v] == none; },
                    [&](v_id_t v, auto&& range) {
        graph_t::any_range(range, [&](v_id_t u) {
          if (!bitmap[u]) return false;
          result.parent[v] = u; // Exclusive destination ownership: no CAS needed.
          result.distance[v] = level + 1;
          // A directed degree-one vertex can still lead to an unvisited vertex.
          if (g.degree(v) > 0) output.add(v);
          return true;
        });
      });
    } else {
      g.active(frontier, [&](v_id_t u, auto&& range) {
        graph_t::map_range(range, [&](v_id_t v) {
          auto parent = atom(result.parent[v]);
          if (parent.load(std::memory_order_relaxed) != none) return;
          v_id_t expected = none;
          if (parent.compare_exchange_strong(expected, u, std::memory_order_relaxed)) {
            result.distance[v] = level + 1;
            if (g.degree(v) > 0) output.add(v);
          }
        });
      }, level & 1);
    }
    output.collect(next);
    frontier.swap(next);
    ++level;
  }
  return result;
}

std::vector<v_id_t> connected_components(const graph_t& g) {
  std::vector<v_id_t> parents(g.n());
  std::iota(parents.begin(), parents.end(), 0);
  auto find = [&](v_id_t v) {
    for (;;) {
      auto p = atom(parents[v]).load(std::memory_order_relaxed);
      if (p == v) return v;
      auto gp = atom(parents[p]).load(std::memory_order_relaxed);
      atom(parents[v]).compare_exchange_weak(p, gp, std::memory_order_relaxed);
      v = p;
    }
  };
  g.vertices([&](v_id_t u, auto&& range) {
    graph_t::map_range(range, [&](v_id_t v) {
      auto a = find(u), b = find(v);
      while (a != b) {
        if (a < b) std::swap(a, b);
        auto expected = a;
        if (atom(parents[a]).compare_exchange_strong(expected, b, std::memory_order_relaxed)) break;
        a = find(a); b = find(b);
      }
    });
  });
  tbb::parallel_for(size_t(0), g.n(), [&](size_t v) {
    auto root = find(v);
    atom(parents[v]).store(root, std::memory_order_relaxed);
  });
  return parents;
}

rank_result_t pagerank(const graph_t& g, size_t max_iterations, double damping, double epsilon) {
  if (!(damping >= 0 && damping < 1) || !(epsilon >= 0))
    throw std::invalid_argument("Invalid PageRank parameters");
  rank_result_t result{std::vector<double>(g.n(), g.n() ? 1.0 / g.n() : 0), 0};
  if (!g.n()) return result;
  std::vector<double> next(g.n()), contribution(g.n());
  while (result.iterations < max_iterations) {
    // Read old ranks only. BYO's in-place p_div update is not safe Jacobi iteration.
    const double dangling = sum<double>(g.n(), 0, [&](size_t v) {
      contribution[v] = g.degree(v) ? result.rank[v] / g.degree(v) : 0;
      return g.degree(v) ? 0.0 : result.rank[v];
    });
    const double constant = (1 - damping + damping * dangling) / g.n();
    g.incoming([&](v_id_t v, auto&& range) {
      double value = 0;
      graph_t::map_range(range, [&](v_id_t u) { value += contribution[u]; });
      next[v] = constant + damping * value;
    }, g.optimized() && !g.fits() && (result.iterations & 1));
    auto difference = sum<double>(g.n(), 0,
        [&](size_t v) { return std::abs(next[v] - result.rank[v]); });
    result.rank.swap(next);
    ++result.iterations;
    if (epsilon > 0 && difference < epsilon) break;
  }
  return result;
}

std::vector<double> betweenness(const graph_t& g, v_id_t source) {
  // Brandes forward levels/path counts and backward dependency accumulation.
  auto traversal = bfs(g, source);
  std::vector<std::vector<v_id_t>> levels;
  for (v_id_t v = 0; v < g.n(); ++v) if (traversal.distance[v] != none) {
    if (levels.size() <= traversal.distance[v]) levels.resize(traversal.distance[v] + 1);
    levels[traversal.distance[v]].push_back(v);
  }
  std::vector<double> paths(g.n(), 0), dependency(g.n(), 0);
  paths[source] = 1;
  for (size_t level = 1; level < levels.size(); ++level) {
    g.active_in(levels[level], [&](v_id_t v, auto&& range) {
      double value = 0;
      graph_t::map_range(range, [&](v_id_t u) {
        if (traversal.distance[u] == level - 1) value += paths[u];
      });
      if (!std::isfinite(value)) throw std::overflow_error("Brandes path count overflow");
      paths[v] = value;
    });
  }
  for (size_t level = levels.size(); level-- > 0;) {
    g.active(levels[level], [&](v_id_t v, auto&& range) {
      double value = 0;
      graph_t::map_range(range, [&](v_id_t u) {
        if (traversal.distance[u] == level + 1)
          value += (paths[v] / paths[u]) * (1 + dependency[u]);
      });
      dependency[v] = value;
    });
  }
  // Includes the source dependency as upstream does; unreachable vertices are zero.
  return dependency;
}

std::vector<uint32_t> coloring(const graph_t& g, uint64_t seed, bool largest_first) {
  if (!g.projected()) { graph_t projection(g.db(), false, true); return coloring(projection, seed, largest_first); }
  std::vector<uint64_t> priorities(g.n());
  std::vector<uint32_t> colors(g.n(), none);
  std::vector<uint64_t> pending(g.n());
  for (size_t v = 0; v < g.n(); ++v) priorities[v] = hash(v ^ seed);
  auto higher = [&](v_id_t a, v_id_t b) {
    const auto da = largest_first ? g.degree(a) : std::bit_width(g.degree(a) ? g.degree(a) - 1 : 0);
    const auto db = largest_first ? g.degree(b) : std::bit_width(g.degree(b) ? g.degree(b) - 1 : 0);
    if (da != db) return da > db;
    return priorities[a] != priorities[b] ? priorities[a] > priorities[b] : a > b;
  };
  g.vertices([&](v_id_t v, auto&& range) {
    graph_t::map_range(range, [&](v_id_t u) {
      if (u == v) throw std::invalid_argument("Coloring requires a loop-free graph");
      pending[v] += higher(u, v);
    });
  });
  std::vector<v_id_t> frontier, next;
  for (size_t v = 0; v < g.n(); ++v) if (!pending[v]) frontier.push_back(v);
  frontier_t output;
  tbb::enumerable_thread_specific<std::vector<uint8_t>> scratch;
  while (!frontier.empty()) {
    g.active(frontier, [&](v_id_t v, auto&& range) {
      auto& used = scratch.local();
      used.assign(g.degree(v) + 1, 0);
      graph_t::map_range(range, [&](v_id_t u) { if (colors[u] < used.size()) used[colors[u]] = 1; });
      colors[v] = std::find(used.begin(), used.end(), 0) - used.begin();
    });
    output.clear();
    g.active(frontier, [&](v_id_t v, auto&& range) {
      graph_t::map_range(range, [&](v_id_t u) {
        if (higher(v, u) && atom(pending[u]).fetch_sub(1, std::memory_order_relaxed) == 1)
          output.add(u);
      });
    });
    output.collect(next); frontier.swap(next);
  }
  return colors;
}

std::vector<uint64_t> kcore(const graph_t& g) {
  if (!g.projected()) { graph_t projection(g.db(), false, true); return kcore(projection); }
  std::vector<uint64_t> degree(g.n()), core(g.n());
  std::vector<uint8_t> alive(g.n(), 1);
  for (size_t v = 0; v < g.n(); ++v) degree[v] = g.degree(v);
  size_t remaining = g.n();
  std::vector<v_id_t> frontier, next;
  frontier_t output;
  while (remaining) {
    const auto k = tbb::parallel_reduce(tbb::blocked_range<size_t>(0, g.n(), 2048), UINT64_MAX,
        [&](const auto& r, uint64_t value) {
          for (size_t v = r.begin(); v < r.end(); ++v) if (alive[v]) value = std::min(value, degree[v]);
          return value;
        }, [](auto a, auto b) { return std::min(a, b); });
    frontier.clear();
    for (size_t v = 0; v < g.n(); ++v) if (alive[v] && degree[v] == k) frontier.push_back(v);
    while (!frontier.empty()) {
      for (auto v : frontier) { alive[v] = 0; core[v] = k; }
      remaining -= frontier.size();
      output.clear();
      g.active(frontier, [&](v_id_t, auto&& range) {
        graph_t::map_range(range, [&](v_id_t u) {
          if (!alive[u]) return;
          auto d = atom(degree[u]).load(std::memory_order_relaxed);
          while (d > k) {
            if (atom(degree[u]).compare_exchange_weak(d, d - 1, std::memory_order_relaxed)) {
              if (d == k + 1) output.add(u);
              break;
            }
          }
        });
      });
      output.collect(next); frontier.swap(next);
    }
  }
  return core;
}

std::vector<uint8_t> maximal_independent_set(const graph_t& g, uint64_t seed) {
  if (!g.projected()) { graph_t projection(g.db(), false, true); return maximal_independent_set(projection, seed); }
  // Random-priority greedy MIS. Each round's candidates form an independent set.
  std::vector<uint8_t> state(g.n(), 0), selected(g.n(), 0);
  std::vector<uint64_t> priority(g.n());
  for (size_t v = 0; v < g.n(); ++v) priority[v] = hash(v ^ seed);
  for (;;) {
    frontier_t candidates;
    g.vertices([&](v_id_t v, auto&& range) {
      if (state[v]) return;
      const bool blocked = graph_t::any_range(range, [&](v_id_t u) {
        if (u == v) throw std::invalid_argument("MIS requires a loop-free graph");
        return !state[u] && (priority[u] > priority[v] || (priority[u] == priority[v] && u > v));
      });
      if (!blocked) candidates.add(v);
    });
    std::vector<v_id_t> roots; candidates.collect(roots);
    if (roots.empty()) break;
    for (auto v : roots) { state[v] = 1; selected[v] = 1; }
    g.active(roots, [&](v_id_t, auto&& range) {
      graph_t::map_range(range, [&](v_id_t u) { atom(state[u]).store(2, std::memory_order_relaxed); });
    });
  }
  return selected;
}

ldd_result_t low_diameter_decomposition(const graph_t& g, double beta, uint64_t seed, bool permute) {
  if (!g.projected()) { graph_t projection(g.db(), false, true); return low_diameter_decomposition(projection, beta, seed, permute); }
  if (!(beta > 0) || !std::isfinite(beta)) throw std::invalid_argument("LDD beta must be positive");
  ldd_result_t result{std::vector<v_id_t>(g.n(), none), std::vector<v_id_t>(g.n(), none)};
  std::vector<v_id_t> order(g.n()); std::iota(order.begin(), order.end(), 0);
  if (permute) { std::mt19937_64 rng(seed); std::shuffle(order.begin(), order.end(), rng); }
  std::vector<v_id_t> frontier, next;
  frontier_t output;
  size_t added = 0, visited = 0, round = 0;
  while (visited < g.n()) {
    const double batch = std::exp(std::min(700.0, beta * round));
    size_t count = batch >= g.n() - added ? g.n() - added : size_t(batch);
    for (size_t i = 0; i < count; ++i) {
      const auto v = order[added++];
      if (result.cluster[v] == none) {
        result.cluster[v] = v; result.parent[v] = v; frontier.push_back(v);
      }
    }
    visited += frontier.size();
    if (visited == g.n()) break;
    output.clear();
    g.active(frontier, [&](v_id_t u, auto&& range) {
      const auto cluster = atom(result.cluster[u]).load(std::memory_order_relaxed);
      graph_t::map_range(range, [&](v_id_t v) {
        v_id_t expected = none;
        if (atom(result.cluster[v]).compare_exchange_strong(expected, cluster,
                                                            std::memory_order_relaxed)) {
          result.parent[v] = u; output.add(v);
        }
      });
    });
    output.collect(next); frontier.swap(next); ++round;
  }
  return result;
}

std::vector<edge_t> spanner(const graph_t& g, size_t k, uint64_t seed) {
  if (!g.projected()) { graph_t projection(g.db(), false, true); return spanner(projection, k, seed); }
  if (!k) throw std::invalid_argument("Spanner k must be positive");
  if (g.n() < 2) return {};
  auto ldd = low_diameter_decomposition(g, std::log(g.n()) / (2.0 * k), seed, true);
  // One representative edge per cluster pair plus each cluster's BFS forest.
  tbb::enumerable_thread_specific<folly::F14FastMap<uint64_t, edge_t>> maps;
  g.vertices([&](v_id_t u, auto&& range) {
    auto& local = maps.local();
    graph_t::map_range(range, [&](v_id_t v) {
      auto a = ldd.cluster[u], b = ldd.cluster[v];
      if (a < b) local.try_emplace((uint64_t(a) << 32) | b, u, v);
    });
  });
  folly::F14FastMap<uint64_t, edge_t> unique;
  for (auto& local : maps) for (auto [key, edge] : local) unique.try_emplace(key, edge);
  std::vector<edge_t> edges;
  for (v_id_t v = 0; v < g.n(); ++v) if (ldd.parent[v] != v) edges.emplace_back(v, ldd.parent[v]);
  for (auto [key, edge] : unique) { (void)key; edges.push_back(edge); }
  return edges;
}

uint64_t triangle_count(const graph_t& g) {
  if (!g.projected()) { graph_t projection(g.db(), false, true); return triangle_count(projection); }
  // Degree/ID orientation (Shun-Tangwongsan). Only a worker-local forward
  // neighborhood is materialized; never construct a second whole-graph CSR.
  tbb::enumerable_thread_specific<folly::F14FastSet<v_id_t>> sets;
  tbb::enumerable_thread_specific<uint64_t> counts(0);
  auto less = [&](v_id_t a, v_id_t b) {
    return g.degree(a) != g.degree(b) ? g.degree(a) < g.degree(b) : a < b;
  };
  tbb::parallel_for(tbb::blocked_range<size_t>(0, g.n(), 64), [&](const auto& r) {
    auto& forward = sets.local();
    auto& count = counts.local();
    for (size_t u = r.begin(); u < r.end(); ++u) {
      forward.clear();
      g.map(u, [&](v_id_t v) { if (less(u, v)) forward.insert(v); });
      // The outer page guard has been released before fetching another vertex.
      for (auto v : forward) g.map(v, [&](v_id_t w) {
        if (less(v, w) && forward.contains(w)) ++count;
      });
    }
  });
  uint64_t total = 0;
  for (auto count : counts) total += count;
  return total;
}

double approximate_densest_subgraph(const graph_t& g, double epsilon) {
  if (!g.projected()) { graph_t projection(g.db(), false, true); return approximate_densest_subgraph(projection, epsilon); }
  if (!(epsilon >= 0) || !std::isfinite(epsilon)) throw std::invalid_argument("Invalid density epsilon");
  std::vector<uint64_t> degree(g.n());
  std::vector<uint8_t> alive(g.n(), 1);
  for (size_t v = 0; v < g.n(); ++v) degree[v] = g.degree(v);
  size_t remaining = g.n();
  double best = 0;
  while (remaining) {
    const auto edges = sum<uint64_t>(g.n(), 0, [&](size_t v) { return alive[v] ? degree[v] : 0; });
    const double density = double(edges) / remaining;
    best = std::max(best, density);
    const double threshold = (1 + epsilon) * density;
    std::vector<v_id_t> removed;
    for (size_t v = 0; v < g.n(); ++v) if (alive[v] && degree[v] <= threshold) {
      removed.push_back(v); alive[v] = 0;
    }
    remaining -= removed.size();
    g.active(removed, [&](v_id_t, auto&& range) {
      graph_t::map_range(range, [&](v_id_t u) {
        if (alive[u]) atom(degree[u]).fetch_sub(1, std::memory_order_relaxed);
      });
    });
  }
  return best;
}

uint64_t map_sum(const graph_t& g) {
  tbb::enumerable_thread_specific<uint64_t> totals(0);
  g.vertices([&](v_id_t, auto&& range) {
    auto& value = totals.local();
    graph_t::map_range(range, [&](v_id_t v) { value += v; });
  });
  uint64_t value = 0;
  for (auto x : totals) value += x;
  return value;
}
namespace {
template <size_t Width> struct alignas(Width) remote_word_t {
  using scalar_t = std::conditional_t<Width == 1, uint8_t,
      std::conditional_t<Width == 2, uint16_t, std::conditional_t<Width == 4, uint32_t, uint64_t>>>;
  scalar_t value{0};
};
template <size_t Width> uint64_t remote_sum(const graph_t& g, v_id_t selected) {
  std::vector<remote_word_t<Width>> data(g.n());
  data[selected].value = 1;
  tbb::enumerable_thread_specific<uint64_t> totals(0);
  g.vertices([&](v_id_t, auto&& range) {
    auto& value = totals.local();
    graph_t::map_range(range, [&](v_id_t v) { value += data[v].value; });
  });
  uint64_t value = 0;
  for (auto x : totals) value += x;
  return value;
}
}
uint64_t map_remote(const graph_t& g, size_t width, v_id_t selected) {
  source_check(g, selected);
  switch (width) {
  case 1: return remote_sum<1>(g, selected);
  case 2: return remote_sum<2>(g, selected);
  case 4: return remote_sum<4>(g, selected);
  case 8: return remote_sum<8>(g, selected);
  case 16: return remote_sum<16>(g, selected);
  case 32: return remote_sum<32>(g, selected);
  case 64: return remote_sum<64>(g, selected);
  case 0: break;
  default: throw std::invalid_argument("Remote width must be 0,1,2,4,8,16,32,64");
  }
  std::vector<uint8_t> bits((g.n() + 7) / 8);
  bits[selected / 8] = uint8_t(1u << (selected % 8));
  tbb::enumerable_thread_specific<uint64_t> totals(0);
  g.vertices([&](v_id_t, auto&& range) {
    auto& value = totals.local();
    graph_t::map_range(range, [&](v_id_t v) { value += (bits[v / 8] >> (v % 8)) & 1; });
  });
  uint64_t value = 0;
  for (auto x : totals) value += x;
  return value;
}
} // namespace bw_graph::byo
