#include "runtime/realtime.hpp"
#include <condition_variable>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace reaweb {
namespace {
class NativeScheduler final : public RealtimeScheduler {
  std::thread worker_;
#ifdef _WIN32
  HANDLE timer_ = nullptr, stop_ = nullptr;
#else
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopped_ = true;
#endif
public:
  ~NativeScheduler() override { stop(); }
  bool supported() const override { return true; }
  void start(double hz, std::function<void()> callback) override {
    stop();
#ifdef _WIN32
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!timer_ || !stop_) { stop(); throw Error("REALTIME_UNAVAILABLE", "Cannot create realtime timer"); }
#else
    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = false; }
#endif
    try {
      worker_ = std::thread([this, hz, callback = std::move(callback)] {
        using Clock = std::chrono::steady_clock;
        const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1 / hz));
        auto next = Clock::now();
        for (;;) {
#ifdef _WIN32
          if (WaitForSingleObject(stop_, 0) != WAIT_TIMEOUT) break;
#else
          { std::lock_guard<std::mutex> lock(mutex_); if (stopped_) break; }
#endif
          try { callback(); } catch (...) {}
          next += period;
          const auto now = Clock::now();
          if (next <= now) next += period * ((now - next) / period + 1);
#ifdef _WIN32
          LARGE_INTEGER due{};
          due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(std::chrono::duration<double>(next - Clock::now()).count() * 10000000));
          if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) break;
          HANDLE handles[]{stop_, timer_};
          if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
#else
          std::unique_lock<std::mutex> lock(mutex_);
          if (wake_.wait_until(lock, next, [this] { return stopped_; })) break;
#endif
        }
      });
    } catch (...) { stop(); throw; }
  }
  void stop() override {
#ifdef _WIN32
    if (stop_) SetEvent(stop_);
#else
    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
    wake_.notify_one();
#endif
    if (worker_.joinable()) worker_.join();
#ifdef _WIN32
    if (timer_) { CloseHandle(timer_); timer_ = nullptr; }
    if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
#endif
  }
};
}
std::unique_ptr<RealtimeScheduler> make_realtime_scheduler() { return std::make_unique<NativeScheduler>(); }
}
