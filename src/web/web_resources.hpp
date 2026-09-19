#pragma once
#include "core/core.hpp"

namespace reaweb {
// A read-only loopback origin for one local App directory. HTTP parsing,
// conditional/range requests and streaming are supplied by cpp-httplib.
// There is deliberately no REAPER API or filesystem-write HTTP endpoint.
class WebResources {
public:
  WebResources(const fs::path& root, const fs::path& profile);
  ~WebResources();
  WebResources(const WebResources&) = delete;
  WebResources& operator=(const WebResources&) = delete;
  const std::string& origin() const;
  std::string entry_url(const fs::path& entry) const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
std::string app_identity(const fs::path& root);
}
