#pragma once
#include "bw_graph/storage/s3_store.h"
#include <rocksdb/db.h>
namespace bw_graph {
// Metadata is working RocksDB locally, checkpointed as portable binary KV records on S3.
// A remote dirty marker prevents reopening a partial/unclean cross-store checkpoint.
class s3_graph_t {
public:
  explicit s3_graph_t(const std::filesystem::path& path);
  bool exists() const { return exists_; }
  void restore(rocksdb::DB& db);
  void mark_open();
  void finish(rocksdb::DB& db);
  static void reset(const std::filesystem::path& path);
private:
  std::shared_ptr<s3_store_t> store_;
  std::string prefix_;
  s3_lock_t lock_;
  bool exists_;
  std::vector<std::byte> snapshot_;
};
}
