#include "bw_graph/storage/s3_graph.h"
#include "bw_graph/storage/s3_pages.h"
#include <rocksdb/write_batch.h>
#include <nlohmann/json.hpp>
namespace bw_graph {
namespace {
std::string prefix(const std::filesystem::path& path) { return "graphs/" + s3_path_key(path); }
void check(const rocksdb::Status& status) { if (!status.ok()) throw std::runtime_error(status.ToString()); }
std::string from_binary(const nlohmann::json& value) {
  const auto& bytes = value.get_binary();
  return std::string(bytes.begin(), bytes.end());
}
}
s3_graph_t::s3_graph_t(const std::filesystem::path& path)
    : store_(std::make_shared<s3_store_t>()), prefix_(prefix(path)),
      lock_(store_, prefix_ + "/owner") {
  std::vector<std::byte> dirty;
  if (store_->get(prefix_ + "/dirty", dirty)) throw std::runtime_error("Unclean S3 graph; recovery required");
  exists_ = store_->get(prefix_ + "/metadata", snapshot_);
}
void s3_graph_t::restore(rocksdb::DB& db) {
  if (!exists_) return;
  auto doc = nlohmann::json::from_cbor(reinterpret_cast<const uint8_t*>(snapshot_.data()),
                                     reinterpret_cast<const uint8_t*>(snapshot_.data() + snapshot_.size()));
  if (doc.at("format") != "bw-s3-graph-1") throw std::runtime_error("Incompatible S3 graph metadata");
  rocksdb::WriteBatch batch;
  std::unique_ptr<rocksdb::Iterator> it(db.NewIterator({}));
  for (it->SeekToFirst(); it->Valid(); it->Next()) batch.Delete(it->key());
  check(it->status());
  for (const auto& entry : doc.at("entries")) batch.Put(from_binary(entry.at(0)), from_binary(entry.at(1)));
  rocksdb::WriteOptions options; options.sync = true;
  check(db.Write(options, &batch));
  snapshot_.clear(); snapshot_.shrink_to_fit();
}
void s3_graph_t::mark_open() { store_->put(prefix_ + "/dirty", {}, true); }
void s3_graph_t::finish(rocksdb::DB& db) {
  nlohmann::json doc{{"format", "bw-s3-graph-1"}, {"entries", nlohmann::json::array()}};
  std::unique_ptr<rocksdb::Iterator> it(db.NewIterator({}));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    auto binary = [](const rocksdb::Slice& s) {
      auto data = reinterpret_cast<const uint8_t*>(s.data());
      return nlohmann::json::binary(std::vector<uint8_t>(data, data + s.size()));
    };
    doc["entries"].push_back({binary(it->key()), binary(it->value())});
  }
  check(it->status());
  auto bytes = nlohmann::json::to_cbor(doc);
  store_->put(prefix_ + "/metadata", std::as_bytes(std::span(bytes)));
  store_->erase(prefix_ + "/dirty");
  lock_.release();
}
void s3_graph_t::reset(const std::filesystem::path& path) {
  auto store = std::make_shared<s3_store_t>();
  const auto root = prefix(path);
  s3_lock_t lock(store, root + "/owner");
  std::vector<std::byte> bytes;
  if (store->get(root + "/dirty", bytes)) throw std::runtime_error("Cannot reset unclean S3 graph");
  store->put(root + "/dirty", {}, true);
  if (store->get(root + "/metadata", bytes)) {
    store->put(root + "/history/" + store->unique_id(), bytes);
    store->erase(root + "/metadata");
  }
  s3_pages_t::reset(path / "adjacency");
  store->erase(root + "/dirty");
  lock.release();
}
}
