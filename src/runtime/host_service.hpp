#pragma once
#include "core/core.hpp"
#include "public/reaweb_service.h"
#include <chrono>
#include <mutex>
#include <thread>

namespace reaweb {
class ServiceRegistry {
public:
  using Clock = std::chrono::steady_clock;
  using Reply = std::function<void(Json)>;
  using Event = std::function<void(uint64_t, const std::string&, int, const std::string&, Json)>;
  explicit ServiceRegistry(Event event);
  int add(const char* name, const ReaWeb_ServiceCallbacks* callbacks, uint64_t* handle);
  int remove(uint64_t handle);
  int complete(uint64_t handle, uint64_t request, const char* json, int status, const char* message);
  int emit(uint64_t handle, int window, const char* event, const char* json);
  uint64_t lookup(const std::string& name) const;
  void call(const std::string& name, const std::string& method, const Json& payload, int window, Reply reply);
  void cancel_window(int window);
  void tick(Clock::time_point now = Clock::now(), Clock::time_point deadline = Clock::time_point::max());
  static const char* code(int status);
private:
  struct Service { std::string name; ReaWeb_ServiceCallbacks callbacks; };
  struct Pending { uint64_t service; int window; Reply reply; Clock::time_point deadline; bool completed = false; };
  struct Output { uint64_t service, request; int window; std::string event; Json data; size_t bytes; };
  std::thread::id main_thread_ = std::this_thread::get_id();
  mutable std::mutex mutex_;
  std::map<uint64_t, Service> services_;
  std::map<uint64_t, Pending> pending_;
  std::deque<Output> output_;
  size_t output_bytes_ = 0;
  uint64_t next_service_ = 0, next_request_ = 0;
  Event event_;
  void cancel(int window, uint64_t service, int status);
};
}
