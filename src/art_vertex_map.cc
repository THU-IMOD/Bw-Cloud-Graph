#include "bw_graph/index/vertex_table.h"
#include "OptimisticLockCoupling/Tree.h"
#include <shared_mutex>

namespace bw_graph {
namespace {
void encode_key(uint64_t id, Key& key) {
  key.setKeyLen(8);
  for (unsigned i = 0; i < 8; ++i) key[i] = (id >> (56 - i * 8)) & 255;
}
void load_vertex_key(TID tid, Key& key) {
  encode_key(reinterpret_cast<const art_vertex_leaf_t*>(tid)->key, key);
}
}
struct art_vertex_map_t::impl_t {
  // Upstream OLC has non-atomic node fields. Exclude structural writes from
  // readers under the C++ memory model; version updates never take this lock.
  mutable std::shared_mutex structure;
  ART_OLC::Tree tree{load_vertex_key};
};
art_vertex_map_t::art_vertex_map_t() : impl_(std::make_unique<impl_t>()) {}
art_vertex_map_t::~art_vertex_map_t() = default;
void art_vertex_map_t::insert(art_vertex_leaf_t* leaf) {
  const auto tid = reinterpret_cast<TID>(leaf);
  if (!tid || (tid >> 63)) throw std::overflow_error("ART leaf pointer cannot be tagged");
  Key key;
  encode_key(leaf->key, key);
  std::unique_lock lock(impl_->structure);
  auto info = impl_->tree.getThreadInfo();
  if (impl_->tree.lookup(key, info)) throw std::logic_error("Duplicate ART vertex ID");
  impl_->tree.insert(key, tid, info);
}
void* art_vertex_map_t::find(uint64_t id) const {
  Key key;
  encode_key(id, key);
  std::shared_lock lock(impl_->structure);
  auto info = impl_->tree.getThreadInfo();
  const auto tid = impl_->tree.lookup(key, info);
  return tid ? reinterpret_cast<const art_vertex_leaf_t*>(tid)->value : nullptr;
}
}
