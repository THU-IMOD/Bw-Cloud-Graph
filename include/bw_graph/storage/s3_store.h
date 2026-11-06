#pragma once
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <stdexcept>
#include <vector>

namespace bw_graph {
class s3_http_error_t : public std::runtime_error {
public:
  long status;
  s3_http_error_t(const char* operation, long code)
      : std::runtime_error(std::string("S3 ") + operation + " status " + std::to_string(code)), status(code) {}
};
// Runtime opt-in. Secrets are read only from the AWS environment, never serialized.
bool s3_enabled();
std::string s3_path_key(const std::filesystem::path& path);
class s3_store_t {
public:
  s3_store_t();
  ~s3_store_t();
  s3_store_t(const s3_store_t&) = delete;
  // Missing objects return false; every other unsuccessful status throws.
  bool get(const std::string& key, std::vector<std::byte>& bytes) const;
  void get_into(const std::string& key, std::span<std::byte> bytes) const;
  void put(const std::string& key, std::span<const std::byte> bytes, bool exclusive = false) const;
  void erase(const std::string& key) const;
  std::string unique_id() const;
private:
  struct impl_t;
  std::unique_ptr<impl_t> impl_;
};
// Non-expiring remote ownership token. Unclean shutdown deliberately requires recovery;
// never steal a lock on a timeout, since a paused writer could still be active.
class s3_lock_t {
public:
  s3_lock_t(std::shared_ptr<s3_store_t> store, std::string key);
  ~s3_lock_t();
  void release();
private:
  std::shared_ptr<s3_store_t> store_;
  std::string key_;
  bool held_{false};
};
}
