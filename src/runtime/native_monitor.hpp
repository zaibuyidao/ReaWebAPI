#pragma once
#include "core/core.hpp"
#include <chrono>
#include <optional>
#include <set>

namespace reaweb {
class NativeMonitorManager {
public:
  using Clock = std::chrono::steady_clock;
  using Read = std::function<std::optional<Json>(Clock::time_point)>;
  using Emit = std::function<void(const std::string&, Json)>;
  void add(std::string name, Read read, std::function<uint64_t()> revision = {}, bool invalidate = false, std::function<void()> reset = {});
  bool contains(const std::string& name) const;
  Json snapshot(const std::string& name) const;
  void subscriptions(const std::map<std::string, size_t>& counts);
  void tick(uint64_t epoch, Clock::time_point deadline, const Emit& emit);
private:
  struct Monitor {
    Read read;
    std::function<uint64_t()> source_revision;
    std::function<void()> reset;
    bool invalidate = false;
    size_t subscribers = 0;
    uint64_t source = 0, revision = 0, epoch = 0;
    Json previous, event;
    Clock::time_point next{};
  };
  std::map<std::string, Monitor> monitors_;
  std::string cursor_;
};
void register_native_monitors(NativeMonitorManager& manager, const Host& host);
}
