#pragma once
#include "web/web_resources.hpp"

namespace reaweb {
class LoopbackOrigin {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  LoopbackOrigin(const fs::path& profile, const std::string& app_id, std::shared_ptr<WebResources> resources);
  ~LoopbackOrigin();
  void attach(const std::shared_ptr<WebResources>& resources);
};
}
