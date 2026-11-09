#include "video/scheduler.hpp"
#include "runtime/realtime.hpp"
#include <atomic>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#else
#include <dlfcn.h>
#endif

namespace reaweb {
namespace {
class MainScheduler final : public VideoScheduler {
  std::function<void()> callback_;
#ifdef _WIN32
  HWND window_ = nullptr;
  HMODULE module_ = nullptr;
  bool registered_ = false;
  inline static unsigned registrations_ = 0;
  inline static bool owns_class_ = false;
  std::unique_ptr<RealtimeScheduler> clock_;
  std::atomic<bool> queued_{false};
  static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<MainScheduler*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      self = static_cast<MainScheduler*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_APP + 31 && self) {
      self->queued_ = false;
      self->invoke();
      return 0;
    }
    return DefWindowProcW(window, message, w, l);
  }
#elif defined(__APPLE__)
  CFRunLoopTimerRef timer_ = nullptr;
#else
  unsigned timer_ = 0;
  using Add = unsigned(*)(int, unsigned, int(*)(void*), void*, void(*)(void*));
  using Remove = int(*)(unsigned);
  Remove remove_ = nullptr;
#endif
  void invoke() {
    const auto callback = callback_;
    try { if (callback) callback(); } catch (...) {}
  }
public:
  ~MainScheduler() override { stop(); }
  void start(std::function<void()> callback) override {
    stop(); callback_ = std::move(callback);
    try {
#ifdef _WIN32
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&procedure), &module_);
      WNDCLASSW type{}; type.lpfnWndProc = procedure; type.hInstance = module_; type.lpszClassName = L"ReaWebVideoClock";
      if (!registrations_) {
        owns_class_ = RegisterClassW(&type) != 0;
        if (!owns_class_ && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw Error("VIDEO_UNAVAILABLE", "Cannot register video clock");
      }
      ++registrations_; registered_ = true;
      window_ = CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module_, this);
      if (!window_) throw Error("VIDEO_UNAVAILABLE", "Cannot create video clock");
      clock_ = make_realtime_scheduler();
      clock_->start(240, [this] {
        if (!queued_.exchange(true) && !PostMessageW(window_, WM_APP + 31, 0, 0)) queued_ = false;
      });
#elif defined(__APPLE__)
      CFRunLoopTimerContext context{0, this, nullptr, nullptr, nullptr};
      timer_ = CFRunLoopTimerCreate(nullptr, CFAbsoluteTimeGetCurrent(), 1.0 / 240, 0, 0,
        [](CFRunLoopTimerRef, void* p) { static_cast<MainScheduler*>(p)->invoke(); }, &context);
      if (!timer_) throw Error("VIDEO_UNAVAILABLE", "Cannot create video clock");
      CFRunLoopAddTimer(CFRunLoopGetMain(), timer_, kCFRunLoopCommonModes);
#else
      const auto add = reinterpret_cast<Add>(dlsym(RTLD_DEFAULT, "g_timeout_add_full"));
      remove_ = reinterpret_cast<Remove>(dlsym(RTLD_DEFAULT, "g_source_remove"));
      if (!add || !remove_) throw Error("VIDEO_UNAVAILABLE", "Main-thread video clock is unavailable");
      timer_ = add(0, 4, [](void* p) -> int { static_cast<MainScheduler*>(p)->invoke(); return 1; }, this, nullptr);
      if (!timer_) throw Error("VIDEO_UNAVAILABLE", "Cannot create video clock");
#endif
    } catch (...) { stop(); throw; }
  }
  void stop() override {
#ifdef _WIN32
    if (clock_) { clock_->stop(); clock_.reset(); }
    if (window_) { DestroyWindow(window_); window_ = nullptr; }
    if (registered_) {
      registered_ = false;
      if (!--registrations_ && owns_class_) { UnregisterClassW(L"ReaWebVideoClock", module_); owns_class_ = false; }
    }
    queued_ = false;
#elif defined(__APPLE__)
    if (timer_) { CFRunLoopTimerInvalidate(timer_); CFRelease(timer_); timer_ = nullptr; }
#else
    if (timer_ && remove_) { remove_(timer_); timer_ = 0; }
#endif
    callback_ = {};
  }
};
}
std::unique_ptr<VideoScheduler> make_video_scheduler() { return std::make_unique<MainScheduler>(); }
}
