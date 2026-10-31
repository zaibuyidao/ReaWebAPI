#pragma once
#include <X11/X.h>
#include <cstdint>
#include <dlfcn.h>

namespace reaweb {
// Observe REAPER's GDK connection without consuming events or loading another GTK.
class LinuxGeometry {
  using Filter = int (*)(void*, void*, void*);
  using Watch = void (*)(void*, Filter, void*);
  using Ref = void* (*)(void*);
  using Unref = void (*)(void*);
  Watch add_ = reinterpret_cast<Watch>(dlsym(RTLD_DEFAULT, "gdk_window_add_filter"));
  Watch remove_ = reinterpret_cast<Watch>(dlsym(RTLD_DEFAULT, "gdk_window_remove_filter"));
  Ref ref_ = reinterpret_cast<Ref>(dlsym(RTLD_DEFAULT, "g_object_ref"));
  Unref unref_ = reinterpret_cast<Unref>(dlsym(RTLD_DEFAULT, "g_object_unref"));
  void* window_ = nullptr;
  uint64_t revision_ = 1;
  static int event(void* native, void*, void* data) {
    if (native) switch (*static_cast<int*>(native)) {
      case ConfigureNotify: case ReparentNotify: case MapNotify: case UnmapNotify:
      case DestroyNotify: case PropertyNotify: case GravityNotify:
        static_cast<LinuxGeometry*>(data)->invalidate();
    }
    return 0; // GDK_FILTER_CONTINUE
  }
public:
  LinuxGeometry() = default;
  LinuxGeometry(const LinuxGeometry&) = delete;
  LinuxGeometry& operator=(const LinuxGeometry&) = delete;
  ~LinuxGeometry() { watch(nullptr); }
  void invalidate() { ++revision_; }
  uint64_t watch(void* window) {
    if (!add_ || !remove_ || !ref_ || !unref_) return ++revision_;
    if (window != window_) {
      if (window_) { remove_(window_, event, this); unref_(window_); }
      window_ = window ? ref_(window) : nullptr;
      if (window_) add_(window_, event, this);
      invalidate();
    }
    return window_ ? revision_ : ++revision_;
  }
};
}
