#pragma once
#include "core/worker.hpp"
#include "public/reaweb_tasks.h"
#include <atomic>
namespace reaweb {
class NativeTasks {
public:
  struct Event { int window; std::string name; Json data; };
  NativeTasks();
  ~NativeTasks();
  uint64_t watch(const fs::path& path, bool recursive, int window);
  void unwatch(uint64_t id, int window);
  int timer(uint32_t delay, uint32_t interval, uint64_t owner, ReaWeb_TimerCallback callback, void* context, uint64_t* handle, int window = 0);
  int cancel(uint64_t id, int window = 0);
  void cancel_window(int window);
  void cancel_owner(uint64_t owner);
  std::vector<Event> tick(Clock::time_point deadline);
private:
  struct Watch;
  struct Timer { Clock::time_point due; uint32_t interval; uint64_t owner; ReaWeb_TimerCallback callback; void* context; int window; };
  std::map<uint64_t, Timer> timers_;
  std::map<uint64_t, std::shared_ptr<Watch>> watches_;
  uint64_t sequence_ = 0;
  std::mutex mutex_;
  std::deque<Event> events_;
  bool stopping_ = false;
  std::condition_variable wake_;
  std::thread thread_;
  const std::thread::id main_ = std::this_thread::get_id();
  void run();
};
}
