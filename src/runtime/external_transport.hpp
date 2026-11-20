#pragma once
#include "core/core.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <vector>

namespace reaweb {
class ExternalConnection {
public:
  struct Request { Json data; std::chrono::steady_clock::time_point received; size_t bytes; bool authenticated; };
  std::atomic<bool> alive{true}, authenticated{false}, closing{false};
  bool take(Request& request, const std::function<bool(const Json&)>& input = {});
  bool send(Json message, bool close = false);
  void close();
private:
  friend class ExternalTransport;
  std::mutex mutex_;
  std::deque<Request> requests_;
  std::deque<std::pair<Json, size_t>> responses_;
  std::set<uint64_t> pending_;
  size_t input_bytes_ = 0, output_bytes_ = 0;
  bool receive(const std::string& text);
  std::string output();
};

// Network I/O and JSON only. Runtime::tick owns all native dispatch and cleanup.
class ExternalTransport {
public:
  explicit ExternalTransport(uint16_t port);
  ~ExternalTransport();
  uint16_t port() const;
  std::vector<std::shared_ptr<ExternalConnection>> connections();
  static constexpr size_t connection_limit = 16, pending_limit = 128;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
