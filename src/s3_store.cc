#include "bw_graph/storage/s3_store.h"
#include <curl/curl.h>
#include <openssl/sha.h>
#include <array>
#include <cerrno>
#include <system_error>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>

namespace bw_graph {
namespace {
std::string env(const char* name, const std::string& fallback = {}) {
  const char* value = std::getenv(name);
  return value && *value ? value : fallback;
}
std::string digest(std::span<const std::byte> bytes) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> value{};
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), value.data());
  std::ostringstream out;
  for (auto b : value) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
  return out.str();
}
std::string escape(const std::string& text) {
  std::ostringstream out;
  for (unsigned char c : text) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
      out << c;
    else out << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
  }
  return out.str();
}
struct sink_t {
  std::vector<std::byte>* dynamic{};
  std::span<std::byte> fixed;
  size_t offset{};
};
size_t receive(char* data, size_t size, size_t count, void* opaque) noexcept {
  auto& sink = *static_cast<sink_t*>(opaque);
  const auto n = size * count;
  try {
    if (sink.dynamic) {
      if (sink.dynamic->size() + n > (size_t(1) << 30)) return 0;
      auto p = reinterpret_cast<std::byte*>(data);
      sink.dynamic->insert(sink.dynamic->end(), p, p + n);
    } else {
      if (n > sink.fixed.size() - sink.offset) return 0;
      std::memcpy(sink.fixed.data() + sink.offset, data, n);
    }
    sink.offset += n;
    return n;
  } catch (...) { return 0; }
}
}
bool s3_enabled() {
  const auto mode = env("BWGRAPH_STORAGE_BACKEND", "local");
  if (mode != "local" && mode != "s3") throw std::invalid_argument("Invalid BWGRAPH_STORAGE_BACKEND");
  return mode == "s3";
}
std::string s3_path_key(const std::filesystem::path& path) {
  auto name = std::filesystem::weakly_canonical(path).generic_string();
  const auto root = env("BWGRAPH_S3_LOCAL_ROOT");
  if (!root.empty()) {
    const auto base = std::filesystem::weakly_canonical(root).generic_string() + '/';
    if (name.starts_with(base)) name = name.substr(base.size());
  }
  return digest(std::as_bytes(std::span(name.data(), name.size())));
}
struct s3_store_t::impl_t {
  std::string endpoint, bucket, prefix, auth, signature, token;
  mutable std::mutex mutex;
  mutable std::vector<CURL*> idle;
  impl_t() {
    static std::once_flag init;
    std::call_once(init, [] {
      if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("curl initialization failed");
    });
    endpoint = env("S3_ENDPOINT"); bucket = env("S3_BUCKET");
    prefix = env("S3_PREFIX", "bw-cloud-graph-java");
    const auto access = env("AWS_ACCESS_KEY_ID"), secret = env("AWS_SECRET_ACCESS_KEY");
    if ((!endpoint.starts_with("http://") && !endpoint.starts_with("https://")) ||
        endpoint.find_first_of("\r\n?#@[]()") != std::string::npos || bucket.empty() ||
        bucket.find_first_of("/\r\n") != std::string::npos || access.empty() || secret.empty())
      throw std::invalid_argument("S3 requires a plain HTTP(S) endpoint, bucket and AWS credentials");
    while (endpoint.ends_with('/')) endpoint.pop_back();
    while (prefix.ends_with('/')) prefix.pop_back();
    if (prefix.empty() || prefix.starts_with('/') || prefix.find("..") != std::string::npos)
      throw std::invalid_argument("S3_PREFIX must be a nonempty relative namespace");
    auth = access + ':' + secret;
    signature = "aws:amz:" + env("AWS_REGION", env("AWS_DEFAULT_REGION", "us-east-1")) + ":s3";
    token = env("AWS_SESSION_TOKEN");
  }
  ~impl_t() { for (auto c : idle) curl_easy_cleanup(c); }
  long request(const char* method, const std::string& key, std::span<const std::byte> source,
               sink_t& sink, bool exclusive = false) const {
    CURL* c;
    { std::lock_guard lock(mutex); c = idle.empty() ? curl_easy_init() : idle.back(); if (!idle.empty()) idle.pop_back(); }
    if (!c) throw std::bad_alloc();
    struct guard_t {
      const impl_t* self; CURL* c; curl_slist* headers{};
      ~guard_t() {
        curl_slist_free_all(headers);
        curl_easy_reset(c);
        try {
          std::lock_guard lock(self->mutex);
          if (self->idle.size() < 64) { self->idle.push_back(c); return; }
        } catch (...) { /* Cleanup must not throw during another exception. */ }
        curl_easy_cleanup(c);
      }
    } guard{this, c};
    auto header = [&](const std::string& value) {
      auto next = curl_slist_append(guard.headers, value.c_str());
      if (!next) throw std::bad_alloc();
      guard.headers = next;
    };
    const std::string url = endpoint + '/' + escape(bucket) + '/' + escape(prefix + '/' + key);
    header("Expect:");
    header("x-amz-content-sha256: " + digest(source));
    if (exclusive) header("If-None-Match: *");
    if (!token.empty()) header("x-amz-security-token: " + token);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(c, CURLOPT_AWS_SIGV4, signature.c_str());
    curl_easy_setopt(c, CURLOPT_USERPWD, auth.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, guard.headers);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L); // Never forward signatures to a redirect.
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    if (std::strcmp(method, "PUT") == 0) {
      curl_easy_setopt(c, CURLOPT_POSTFIELDS, source.empty() ? "" : reinterpret_cast<const char*>(source.data()));
      curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(source.size()));
    }
    const auto code = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    // Deliberately no blind retries of a possibly successful conditional mutation.
    if (code != CURLE_OK) throw std::runtime_error(std::string("S3 ") + method + " transport error " + std::to_string(code));
    return status;
  }
};
s3_store_t::s3_store_t() : impl_(std::make_unique<impl_t>()) {}
s3_store_t::~s3_store_t() = default;
bool s3_store_t::get(const std::string& key, std::vector<std::byte>& bytes) const {
  bytes.clear(); sink_t sink{&bytes, {}, 0};
  auto status = impl_->request("GET", key, {}, sink);
  if (status == 404) { bytes.clear(); return false; }
  if (status != 200) throw std::runtime_error("S3 GET status " + std::to_string(status));
  return true;
}
void s3_store_t::get_into(const std::string& key, std::span<std::byte> bytes) const {
  sink_t sink{nullptr, bytes, 0};
  auto status = impl_->request("GET", key, {}, sink);
  if (status != 200 || sink.offset != bytes.size())
    throw std::runtime_error("S3 page missing, truncated or wrong size; status " + std::to_string(status));
}
void s3_store_t::put(const std::string& key, std::span<const std::byte> bytes, bool exclusive) const {
  std::vector<std::byte> response; sink_t sink{&response, {}, 0};
  auto status = impl_->request("PUT", key, bytes, sink, exclusive);
  if (status != 200) throw s3_http_error_t("PUT", status);
}
void s3_store_t::erase(const std::string& key) const {
  std::vector<std::byte> response; sink_t sink{&response, {}, 0};
  auto status = impl_->request("DELETE", key, {}, sink);
  if (status != 204 && status != 200) throw std::runtime_error("S3 DELETE status " + std::to_string(status));
}
std::string s3_store_t::unique_id() const {
  std::random_device random;
  std::array<uint32_t, 8> words{};
  for (auto& word : words) word = random();
  return digest(std::as_bytes(std::span(words)));
}
s3_lock_t::s3_lock_t(std::shared_ptr<s3_store_t> store, std::string key)
    : store_(std::move(store)), key_(std::move(key)) {
  auto token = store_->unique_id();
  try { store_->put(key_, std::as_bytes(std::span(token.data(), token.size())), true); }
  catch (const s3_http_error_t& error) {
    if (error.status == 412) throw std::system_error(EBUSY, std::generic_category(), "S3 store already owned");
    throw;
  }
  held_ = true;
}
s3_lock_t::~s3_lock_t() { try { release(); } catch (...) {} }
void s3_lock_t::release() { if (held_) { store_->erase(key_); held_ = false; } }
}
