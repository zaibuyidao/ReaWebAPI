#pragma once
#include "runtime/native_stream.hpp"
#include <functional>

namespace reaweb {
// Sources run on a dedicated thread and may call only documented thread-safe APIs.
class RealtimeScheduler {
public:
  virtual ~RealtimeScheduler() = default;
  virtual bool supported() const = 0;
  virtual void start(double hz, std::function<void()> callback) = 0;
  virtual void stop() = 0;
};
std::unique_ptr<RealtimeScheduler> make_realtime_scheduler();

// Source implementations own sampling semantics; the manager owns subscriptions,
// shared sampling, scheduling, publication and cleanup. JSFX is not managed here.
class RealtimeManager {
public:
  RealtimeManager(const Host&, StreamHub&, std::unique_ptr<RealtimeScheduler> = {});
  ~RealtimeManager();
  Json capabilities() const;
  Json diagnostics(int window) const;
  Json subscribe(const std::string& source, const Json& options, int window);
  void unsubscribe(uint64_t id, int window);
  void close_window(int window);
  void housekeeping();
  void shutdown();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
