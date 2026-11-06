#include "bw_graph/algo/async/algorithms.h"
#include <queue>
#include <numeric>

namespace bw_graph::async_algo {
namespace {
constexpr uint32_t none = UINT32_MAX;
template <class T> std::atomic_ref<T> atom(T& x) { return std::atomic_ref<T>(x); }
struct page_t {
  std::mutex mutex;
  v_id_t head{none}, tail{none};
  bool active{false}; // Queued, loading, or executing; one executor owns each page.
};
struct queued_t {
  uint32_t priority, page;
  bool operator<(const queued_t& other) const {
    return std::tie(priority, page) > std::tie(other.priority, other.page);
  }
};

// A page is queued once. Per-vertex intrusive links coalesce repeat updates without
// unbounded duplicate work items. Relaxation state is read afresh on execution.
class worklist_t {
  struct shard_t {
    std::mutex mutex;
    std::priority_queue<queued_t> ready;
    std::atomic<size_t> size{0};
    std::atomic<uint32_t> minimum{UINT32_MAX};
  };
public:
  explicit worklist_t(const byo::graph_t& graph, size_t workers)
      : graph(graph), store(*graph.db().multiscale), links(graph.n(), none), queued(graph.n(), 0) {
    for (size_t c = 0; c < 4; ++c) {
      offsets[c] = count;
      count += store.disk().get_page_count(static_cast<page_space_t>(c));
    }
    if (count >= none) throw std::overflow_error("Async page directory too large");
    pages = std::vector<page_t>(count);
    for (size_t i = 0; i < workers * 2; ++i) {
      auto shard = std::make_unique<shard_t>();
      std::vector<queued_t> storage; storage.reserve((count + workers * 2 - 1) / (workers * 2));
      shard->ready = decltype(shard->ready)(std::less<queued_t>(), std::move(storage));
      shards.push_back(std::move(shard));
    }
  }
  uint32_t key(v_id_t v) const {
    const auto id = store.location(v).first.page;
    return offsets[static_cast<size_t>(id.space())] + id.slot();
  }
  void ready(uint32_t p, uint32_t priority) {
    auto& shard = *shards[p % shards.size()];
    std::lock_guard lock(shard.mutex);
    shard.ready.push({priority, p});
    shard.minimum.store(shard.ready.top().priority, std::memory_order_relaxed);
    shard.size.fetch_add(1, std::memory_order_release);
  }
  template <class Priority> void push(std::span<const v_id_t> ids, Priority&& priority) {
    for (auto v : ids) {
      if (!graph.degree(v) ||
          std::atomic_ref(queued[v]).exchange(1, std::memory_order_acq_rel)) continue;
      auto p = key(v);
      auto& page = pages[p];
      std::lock_guard lock(page.mutex);
      links[v] = none;
      if (page.tail != none) links[page.tail] = v;
      else page.head = v;
      page.tail = v;
      if (!page.active) {
        page.active = true; active.fetch_add(1, std::memory_order_release);
        ready(p, priority(v));
      }
    }
  }
  uint32_t pop(size_t& cursor) {
    size_t best = shards.size(); uint32_t priority = UINT32_MAX;
    for (size_t i = 0; i < shards.size(); ++i) {
      const auto j = cursor++ % shards.size();
      auto& shard = *shards[j];
      if (!shard.size.load(std::memory_order_acquire)) continue;
      auto value = shard.minimum.load(std::memory_order_relaxed);
      if (best == shards.size() || value < priority) { best = j; priority = value; }
    }
    if (best == shards.size()) return none;
    auto& shard = *shards[best];
    std::unique_lock lock(shard.mutex, std::try_to_lock);
    if (!lock || shard.ready.empty()) return none;
    auto p = shard.ready.top().page; shard.ready.pop();
    shard.minimum.store(shard.ready.empty() ? UINT32_MAX : shard.ready.top().priority,
                        std::memory_order_relaxed);
    shard.size.fetch_sub(1, std::memory_order_release); return p;
  }
  size_t take(uint32_t p, std::span<v_id_t> out) {
    auto& page = pages[p];
    std::lock_guard lock(page.mutex); size_t n = 0;
    while (page.head != none && n < out.size()) {
      auto v = page.head; page.head = links[v];
      std::atomic_ref(queued[v]).store(0, std::memory_order_release);
      out[n++] = v;
    }
    if (page.head == none) page.tail = none;
    return n;
  }
  v_id_t first(uint32_t p) {
    auto& page = pages[p]; std::lock_guard lock(page.mutex); return page.head;
  }
  template <class Priority> void finish(uint32_t p, Priority&& priority) {
    auto& page = pages[p]; std::lock_guard lock(page.mutex);
    if (page.head == none) { page.active = false; active.fetch_sub(1, std::memory_order_acq_rel); }
    else ready(p, priority(page.head));
  }
  bool done() const { return !active.load(std::memory_order_acquire) || stopped.load(); }
  void fail(std::exception_ptr e) {
    std::lock_guard lock(error_mutex); if (!error) error = e;
    stopped.store(true, std::memory_order_release);
  }
  void wait() { std::this_thread::yield(); }
  uint64_t bytes() const {
    return links.capacity() * sizeof(v_id_t) + queued.capacity() +
           pages.capacity() * sizeof(page_t) +
           (count + shards.size()) * sizeof(queued_t) + shards.size() * sizeof(shard_t);
  }
  const byo::graph_t& graph;
  multiscale_store_t& store;
  std::exception_ptr error;
private:
  std::vector<v_id_t> links;
  std::vector<uint8_t> queued;
  std::vector<page_t> pages;
  std::array<uint64_t, 4> offsets{};
  uint64_t count{0};
  std::atomic<uint64_t> active{0};
  std::atomic<bool> stopped{false};
  std::vector<std::unique_ptr<shard_t>> shards;
  std::mutex error_mutex;
};

template <class Priority, class Process>
void run(const byo::graph_t& graph, std::span<const v_id_t> initial,
         Priority&& priority, Process&& process, options_t options, stats_t* stats) {
  if (!graph.db().multiscale) throw std::invalid_argument("Async worklist requires native storage");
  if (!options.workers || !options.window)
    throw std::invalid_argument("Positive async options required");
  worklist_t queue(graph, options.workers);
  queue.push(initial, priority);
  auto pool = queue.store.pool().stats(page_space_t::CSR_4K);
  const auto free = pool.frame_count - pool.pinned_frames;
  const size_t window = options.io && !graph.fits() && free > 2 ?
      std::min(options.window, (free - 2) / options.workers) : 0;
  struct task_t { uint32_t page; page_pin_t pin; std::future<page_pin_t> future; };
  std::vector<stats_t> local(options.workers);
  tbb::parallel_for(size_t(0), options.workers, [&](size_t worker) {
    auto& s = local[worker];
    std::vector<task_t> tasks; tasks.reserve(std::max<size_t>(1, window));
    std::array<v_id_t, 1024> vertices;
    std::array<v_id_t, 4096> outgoing;
    size_t out_count = 0, cursor = worker * 2;
    auto flush = [&] { queue.push(std::span(outgoing).first(out_count), priority); out_count = 0; };
    auto activate = [&](v_id_t v) {
      if (!graph.degree(v)) return;
      outgoing[out_count++] = v;
      if (out_count == outgoing.size()) flush();
    };
    try {
      while (!queue.done()) {
        while (tasks.size() < std::max<size_t>(1, window)) {
          auto p = queue.pop(cursor); if (p == none) break;
          task_t t{p, {}, {}};
          const auto id = queue.store.location(queue.first(p)).first.page;
          if (window && id.space() == page_space_t::CSR_4K) {
            t.pin = queue.store.pool().try_pin_page(id);
            if (t.pin) ++s.cache_hits;
            else { t.future = queue.store.pool().fetch_async(id).result; ++s.async_reads; }
          }
          tasks.push_back(std::move(t));
        }
        bool progressed = false;
        for (size_t i = 0; i < tasks.size();) {
          auto& t = tasks[i];
          if (t.future.valid() && t.future.wait_for(std::chrono::seconds(0)) !=
                                    std::future_status::ready) { ++i; continue; }
          if (t.future.valid()) t.pin = t.future.get();
          // Bounded local closure: exploit updates that return to this resident page.
          for (size_t wave = 0; wave < 8; ++wave) {
            const auto n = queue.take(t.page, vertices);
            if (!n) break;
            queue.store.visit_base_group(std::span(vertices).first(n),
                [&](v_id_t v, auto&& range) {
              ++s.vertices; s.edges += range.size();
              process(v, range, activate);
            });
            flush();
          }
          queue.finish(t.page, priority); ++s.tasks;
          if (i + 1 != tasks.size()) t = std::move(tasks.back());
          tasks.pop_back(); progressed = true;
        }
        if (!progressed) {
          const auto start = std::chrono::steady_clock::now();
          if (tasks.empty()) queue.wait(); else tasks.front().future.wait();
          s.wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - start).count();
        }
      }
    } catch (...) { queue.fail(std::current_exception()); }
    // Join accepted reads before reporting a failure or releasing the caller's store.
    for (auto& t : tasks) if (t.future.valid()) {
      try { t.pin = t.future.get(); } catch (...) { queue.fail(std::current_exception()); }
    }
  });
  if (stats) {
    for (auto& s : local) stats->add(s);
    stats->auxiliary_bytes = std::max(stats->auxiliary_bytes, queue.bytes() +
        options.workers * (5120 * sizeof(v_id_t) + std::max<size_t>(1, window) * sizeof(task_t)));
  }
  if (queue.error) std::rethrow_exception(queue.error);
}
}

byo::bfs_result_t bfs(const byo::graph_t& graph, v_id_t source,
                     options_t options, stats_t* stats) {
  if (source >= graph.n()) throw std::out_of_range("Async BFS source");
  // Atomic (distance,parent) avoids publishing a stale parent after a shorter path wins.
  std::vector<uint64_t> state(graph.n(), UINT64_MAX);
  state[source] = source;
  auto priority = [&](v_id_t v) {
    return uint32_t(atom(state[v]).load(std::memory_order_relaxed) >> 32);
  };
  run(graph, std::span<const v_id_t>(&source, 1), priority,
      [&](v_id_t v, neighbor_range_t& range, auto&& activate) {
    const uint64_t next = uint64_t(priority(v) + 1) << 32 | v;
    byo::graph_t::map_range(range, [&](v_id_t u) {
      auto old = atom(state[u]).load(std::memory_order_relaxed);
      while ((old >> 32) > (next >> 32)) {
        if (atom(state[u]).compare_exchange_weak(old, next, std::memory_order_relaxed)) {
          if (graph.degree(u) > 0) activate(u);
          break;
        }
      }
    });
  }, options, stats);
  byo::bfs_result_t result{std::vector<v_id_t>(graph.n()), std::vector<uint32_t>(graph.n())};
  tbb::parallel_for(size_t(0), graph.n(), [&](size_t v) {
    result.parent[v] = uint32_t(state[v]); result.distance[v] = state[v] >> 32;
  });
  if (stats) stats->auxiliary_bytes += state.capacity() * sizeof(uint64_t);
  return result;
}

std::vector<v_id_t> connected_components(const byo::graph_t& graph,
                                        options_t options, stats_t* stats) {
  // Directional label propagation alone cannot compute weak connectivity.
  // Use the union kernel until the page scheduler supports both directions.
  if (graph.db().multiscale) {
    if (stats) *stats = {};
    return byo::connected_components(graph);
  }
  std::vector<v_id_t> labels(graph.n()), initial(graph.n());
  std::iota(labels.begin(), labels.end(), 0); std::iota(initial.begin(), initial.end(), 0);
  auto priority = [&](v_id_t v) { return atom(labels[v]).load(std::memory_order_relaxed) >> 4; };
  run(graph, initial, priority, [&](v_id_t v, neighbor_range_t& range, auto&& activate) {
    const auto label = atom(labels[v]).load(std::memory_order_relaxed);
    byo::graph_t::map_range(range, [&](v_id_t u) {
      auto old = atom(labels[u]).load(std::memory_order_relaxed);
      while (old > label) {
        if (atom(labels[u]).compare_exchange_weak(old, label, std::memory_order_relaxed)) {
          activate(u); break;
        }
      }
    });
  }, options, stats);
  if (stats) stats->auxiliary_bytes += initial.capacity() * sizeof(v_id_t);
  return labels;
}
} // namespace bw_graph::async_algo
