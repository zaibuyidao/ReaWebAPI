#pragma once
#include "core/core.hpp"
#include <mutex>
#include <atomic>

namespace reaweb {
struct ResourceReader {
  virtual ~ResourceReader() = default;
  virtual size_t read(uint64_t offset, char* output, size_t count) = 0;
};
struct ResourceResponse {
  int status = 200;
  std::map<std::string, std::string> headers;
  // Retain the mapped file until the native WebView has consumed the response.
  std::shared_ptr<const void> owner;
  const char* data = nullptr;
  size_t size = 0;
  std::shared_ptr<ResourceReader> reader;
  uint64_t offset = 0;
  const char* reason() const {
    switch (status) {
      case 200: return "OK";
      case 206: return "Partial Content";
      case 304: return "Not Modified";
      case 400: return "Bad Request";
      case 403: return "Forbidden";
      case 404: return "Not Found";
      case 405: return "Method Not Allowed";
      case 416: return "Range Not Satisfiable";
      case 429: return "Too Many Requests";
      default: return "Internal Server Error";
    }
  }
};
class WebResources {
public:
  WebResources(const fs::path& root, const std::string& app_id);
  ~WebResources();
  const fs::path& root() const { return root_; }
  const std::string& origin() const { return origin_; }
  std::string entry_url(const fs::path& entry) const;
  ResourceResponse request(const std::string& uri, const std::string& method,
    const std::map<std::string, std::string>& headers = {}) const;
  Json open_media(const fs::path& path);
  bool close_media(const std::string& url);
  void clear_media();
private:
  struct Media;
  mutable std::mutex media_mutex_;
  std::map<std::string, std::shared_ptr<Media>> media_;
  std::shared_ptr<std::atomic<size_t>> media_reads_ = std::make_shared<std::atomic<size_t>>(0);
  ResourceResponse media_request(const std::string& token, const std::string& method,
    const std::map<std::string, std::string>& headers) const;
  fs::path root_;
  std::string origin_;
};
std::string app_identity(const fs::path& root, const std::string& launcher_source = {});
// Retain the lease while windows use this binding, including asynchronous startup.
std::shared_ptr<void> bind_app_identity(const fs::path& root, const fs::path& profile,
  const std::string& app_id, const fs::path& entry);
}
