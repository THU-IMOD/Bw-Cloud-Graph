#include "bw_graph/index/multiscale_tat.h"
#include "bw_graph/part/reorder.h"
#include <cstring>
#include <set>
#include <unordered_map>

namespace bw_graph {
multiscale_tat_t::multiscale_tat_t(size_t fanout) : fanout_(fanout) {
  if (fanout < 2 || fanout > 1024) throw std::invalid_argument("TAT fanout");
}
multiscale_tat_t::id_t multiscale_tat_t::make_node(std::vector<id_t> children, id_t parent) {
  auto id = next_++;
  nodes_[id] = {parent, std::move(children), {}};
  for (auto child : nodes_.at(id).children) nodes_.at(child).parent = id;
  return id;
}
void multiscale_tat_t::collect(id_t id, std::vector<v_id_t>& vertices) const {
  const auto& n = nodes_.at(id);
  if (!internal(id)) vertices.insert(vertices.end(), n.vertices.begin(), n.vertices.end());
  else for (auto c : n.children) collect(c, vertices);
}
std::vector<multiscale_tat_t::id_t> multiscale_tat_t::topology_order(
    const std::vector<id_t>& children, const edges_t& edges) const {
  // Build the quotient graph of the children from the current base generation.
  // Pending Delta is deliberately not folded into structural decisions before
  // its visibility watermark. No stale cross-page edge cache to invalidate.
  std::unordered_map<v_id_t, v_id_t> owner;
  std::vector<std::vector<v_id_t>> members(children.size());
  for (size_t i = 0; i < children.size(); ++i) {
    collect(children[i], members[i]);
    for (auto v : members[i]) owner.emplace(v, i);
  }
  adj_map_t graph;
  for (size_t i = 0; i < children.size(); ++i) {
    std::set<v_id_t> neighbors;
    for (auto v : members[i]) for (auto dst : edges(v)) {
      auto it = owner.find(dst);
      if (it != owner.end() && it->second != i) neighbors.insert(it->second);
    }
    graph.emplace(i, std::vector<v_id_t>(neighbors.begin(), neighbors.end()));
  }
  auto order = gorder_reordering_optimized(graph);
  if (order.size() != children.size()) throw std::logic_error("Incomplete TAT Gorder");
  std::vector<id_t> result;
  for (auto i : order) result.push_back(children.at(i));
  return result;
}
void multiscale_tat_t::rebalance(id_t id, const edges_t& edges) {
  while (nodes_.at(id).children.size() > fanout_) {
    const auto parent = nodes_.at(id).parent;
    auto ordered = topology_order(nodes_.at(id).children, edges);
    // Leave slack for subsequent leaf replacements. Split by Gorder sequence,
    // never by the numeric midpoint of page or vertex IDs.
    const size_t target = std::max<size_t>(2, fanout_ * 6 / 10);
    std::vector<id_t> parts;
    for (size_t i = 0; i < ordered.size(); i += target) {
      auto end = std::min(ordered.size(), i + target);
      parts.push_back(make_node({ordered.begin() + i, ordered.begin() + end}, parent));
    }
    nodes_.erase(id); ++splits_;
    if (!parent) {
      root_ = make_node(std::move(parts), 0);
      id = root_;
    } else {
      auto& siblings = nodes_.at(parent).children;
      auto it = std::find(siblings.begin(), siblings.end(), id);
      if (it == siblings.end()) throw std::logic_error("Missing TAT parent link");
      auto position = it - siblings.begin();
      siblings.erase(it);
      siblings.insert(siblings.begin() + position, parts.begin(), parts.end());
      id = parent;
    }
  }
}
void multiscale_tat_t::build(std::vector<leaf_t> leaves, const edges_t& edges) {
  std::lock_guard lock(mutex_);
  if (!nodes_.empty()) throw std::logic_error("TAT already initialized");
  std::vector<id_t> children;
  for (auto& leaf : leaves) {
    if (!nodes_.emplace(leaf.page.raw(), node_t{0, {}, std::move(leaf.vertices)}).second)
      throw std::logic_error("Duplicate TAT leaf");
    children.push_back(leaf.page.raw());
  }
  // Initial leaves already follow KaMinPar/Gorder partition order. Preserve
  // those neighborhoods when building the lower levels, rather than reordering
  // the entire graph a second time.
  while (children.size() > fanout_) {
    std::vector<id_t> parents;
    const size_t target = std::max<size_t>(2, fanout_ * 6 / 10);
    for (size_t i = 0; i < children.size(); i += target)
      parents.push_back(make_node({children.begin() + i,
          children.begin() + std::min(children.size(), i + target)}, 0));
    children = std::move(parents);
  }
  root_ = make_node(std::move(children), 0);
  (void)edges;
  validate_locked();
}
void multiscale_tat_t::replace(page_id_t old, std::vector<leaf_t> leaves,
                              const edges_t& edges) {
  std::lock_guard lock(mutex_);
  if (leaves.empty()) throw std::logic_error("Empty TAT replacement");
  const auto parent = nodes_.at(old.raw()).parent;
  std::vector<v_id_t> before = nodes_.at(old.raw()).vertices, after;
  for (const auto& l : leaves) after.insert(after.end(), l.vertices.begin(), l.vertices.end());
  std::sort(before.begin(), before.end()); std::sort(after.begin(), after.end());
  if (before != after) throw std::logic_error("TAT replacement changed membership");
  std::vector<id_t> ids;
  for (auto& leaf : leaves) {
    if (!nodes_.emplace(leaf.page.raw(), node_t{parent, {}, std::move(leaf.vertices)}).second)
      throw std::logic_error("Duplicate replacement leaf");
    ids.push_back(leaf.page.raw());
  }
  auto& siblings = nodes_.at(parent).children;
  auto it = std::find(siblings.begin(), siblings.end(), old.raw());
  if (it == siblings.end()) throw std::logic_error("Missing TAT leaf link");
  auto position = it - siblings.begin();
  siblings.erase(it); siblings.insert(siblings.begin() + position, ids.begin(), ids.end());
  nodes_.erase(old.raw()); ++replacements_;
  rebalance(parent, edges);
}
void multiscale_tat_t::append(leaf_t leaf, const edges_t& edges) {
  std::lock_guard lock(mutex_);
  auto parent = root_;
  while (!nodes_.at(parent).children.empty() && internal(nodes_.at(parent).children.back()))
    parent = nodes_.at(parent).children.back();
  const auto id = leaf.page.raw();
  if (!nodes_.emplace(id, node_t{parent, {}, std::move(leaf.vertices)}).second)
    throw std::logic_error("Duplicate appended leaf");
  nodes_.at(parent).children.push_back(id);
  rebalance(parent, edges);
}
void multiscale_tat_t::set_members(page_id_t page, std::vector<v_id_t> members) {
  std::lock_guard lock(mutex_);
  nodes_.at(page.raw()).vertices = std::move(members);
}
void multiscale_tat_t::rewrite(std::span<const page_id_t> old, std::vector<leaf_t> leaves,
                             const edges_t& edges) {
  if (old.empty()) {
    for (auto& leaf : leaves) append(std::move(leaf), edges);
    return;
  }
  std::lock_guard lock(mutex_);
  const auto parent = nodes_.at(old.front().raw()).parent;
  for (auto& leaf : leaves) {
    auto id = leaf.page.raw();
    if (!nodes_.emplace(id, node_t{parent, {}, std::move(leaf.vertices)}).second)
      throw std::logic_error("Duplicate rewritten leaf");
    nodes_.at(parent).children.push_back(id);
  }
  for (auto page : old) {
    auto id = id_t(page.raw());
    auto p = nodes_.at(id).parent;
    nodes_.erase(id);
    std::erase(nodes_.at(p).children, id);
    while (p != root_ && nodes_.at(p).children.empty()) {
      auto grand = nodes_.at(p).parent;
      std::erase(nodes_.at(grand).children, p);
      nodes_.erase(p); p = grand;
    }
  }
  ++replacements_;
  if (nodes_.contains(parent)) rebalance(parent, edges);
}
std::vector<v_id_t> multiscale_tat_t::members(page_id_t page) const {
  std::lock_guard lock(mutex_); return nodes_.at(page.raw()).vertices;
}
std::vector<multiscale_tat_t::leaf_t> multiscale_tat_t::leaves() const {
  std::lock_guard lock(mutex_);
  std::vector<leaf_t> result;
  std::function<void(id_t)> visit = [&](id_t id) {
    const auto& n = nodes_.at(id);
    if (!internal(id)) result.push_back({page_id_t::from_raw(id), n.vertices});
    else for (auto c : n.children) visit(c);
  };
  visit(root_); return result;
}
multiscale_tat_t::stats_t multiscale_tat_t::stats() const {
  std::lock_guard lock(mutex_);
  stats_t result{0, 0, 0, replacements_, splits_};
  std::function<void(id_t, size_t)> visit = [&](id_t id, size_t depth) {
    result.height = std::max(result.height, depth);
    if (!internal(id)) ++result.leaves;
    else { ++result.internal; for (auto c : nodes_.at(id).children) visit(c, depth + 1); }
  };
  visit(root_, 1); return result;
}
uint64_t multiscale_tat_t::memory_bytes() const {
  std::lock_guard lock(mutex_);
  uint64_t bytes = sizeof(*this);
  for (const auto& [id, n] : nodes_) {
    (void)id;
    // Container allocator bookkeeping is implementation-dependent; count the
    // payload, tree links and allocated vector capacities explicitly.
    bytes += sizeof(id_t) + sizeof(node_t) + 3 * sizeof(void*) +
             n.children.capacity() * sizeof(id_t) + n.vertices.capacity() * sizeof(v_id_t);
  }
  return bytes;
}
void multiscale_tat_t::validate_locked() const {
  std::set<id_t> seen;
  std::set<v_id_t> vertices;
  size_t leaf_depth = 0;
  std::function<void(id_t, id_t, size_t)> visit = [&](id_t id, id_t parent, size_t depth) {
    if (!seen.insert(id).second || !nodes_.contains(id)) throw std::runtime_error("Cyclic TAT");
    const auto& n = nodes_.at(id);
    if (n.parent != parent) throw std::runtime_error("Invalid TAT parent");
    if (internal(id)) {
      if (n.children.size() > fanout_ || !n.vertices.empty() ||
          (n.children.empty() && id != root_)) throw std::runtime_error("Invalid TAT internal");
      for (auto c : n.children) visit(c, id, depth + 1);
    } else {
      auto page = page_id_t::from_raw(id);
      if (page.space() != page_space_t::CSR_4K || !n.children.empty() || n.vertices.empty())
        throw std::runtime_error("Invalid TAT leaf");
      if (leaf_depth && leaf_depth != depth) throw std::runtime_error("Unbalanced TAT");
      leaf_depth = depth;
      for (auto v : n.vertices) if (!vertices.insert(v).second)
        throw std::runtime_error("Duplicate TAT vertex");
    }
  };
  if (root_ < first_internal || next_ <= root_) throw std::runtime_error("Invalid TAT root");
  visit(root_, 0, 1);
  if (seen.size() != nodes_.size()) throw std::runtime_error("Unreachable TAT node");
}
void multiscale_tat_t::validate() const { std::lock_guard lock(mutex_); validate_locked(); }
void multiscale_tat_t::encode(std::vector<std::byte>& bytes) const {
  std::lock_guard lock(mutex_);
  auto put = [&](uint64_t value) {
    auto pos = bytes.size(); bytes.resize(pos + 8); std::memcpy(bytes.data() + pos, &value, 8);
  };
  put(0x315441544742); put(fanout_); put(root_); put(next_); put(nodes_.size());
  for (const auto& [id, n] : nodes_) {
    put(id); put(n.parent); put(n.children.size()); put(n.vertices.size());
    for (auto c : n.children) put(c);
    for (auto v : n.vertices) put(v);
  }
}
void multiscale_tat_t::decode(std::span<const std::byte> bytes) {
  std::lock_guard lock(mutex_);
  size_t pos = 0;
  auto get = [&]() {
    if (bytes.size() - pos < 8) throw std::runtime_error("Short TAT index");
    uint64_t value; std::memcpy(&value, bytes.data() + pos, 8); pos += 8; return value;
  };
  if (get() != 0x315441544742) throw std::runtime_error("Invalid TAT magic");
  fanout_ = get(); root_ = get(); next_ = get(); auto count = get();
  if (fanout_ < 2 || fanout_ > 1024 || count > bytes.size() / 32)
    throw std::runtime_error("Invalid TAT size");
  nodes_.clear();
  for (size_t i = 0; i < count; ++i) {
    auto id = get(), parent = get(), nc = get(), nv = get();
    if (nc > fanout_ || nv > bytes.size() / 8 || nc + nv > (bytes.size() - pos) / 8)
      throw std::runtime_error("Invalid TAT node size");
    node_t n; n.parent = parent;
    for (size_t c = 0; c < nc; ++c) n.children.push_back(get());
    for (size_t v = 0; v < nv; ++v) {
      auto value = get(); if (value >= UINT32_MAX) throw std::runtime_error("Invalid TAT vertex");
      n.vertices.push_back(value);
    }
    if (id >= next_ || !nodes_.emplace(id, std::move(n)).second)
      throw std::runtime_error("Invalid TAT node ID");
  }
  if (pos != bytes.size()) throw std::runtime_error("Trailing TAT data");
  validate_locked();
}
} // namespace bw_graph
