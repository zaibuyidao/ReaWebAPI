#pragma once
#include "runtime/icon.hpp"
#include <dlfcn.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace reaweb {
// Use the GTK version already loaded by REAPER, without loading another GTK runtime.
class LinuxIcon {
  struct List { void* data; List* next; List* prev; };
  using Create = void* (*)(const unsigned char*, int, int, int, int, int, int, void (*)(unsigned char*, void*), void*);
  using Set = void (*)(void*, List*);
  using Unref = void (*)(void*);
  Create create_ = reinterpret_cast<Create>(dlsym(RTLD_DEFAULT, "gdk_pixbuf_new_from_data"));
  Set set_ = reinterpret_cast<Set>(dlsym(RTLD_DEFAULT, "gdk_window_set_icon_list"));
  Unref unref_ = reinterpret_cast<Unref>(dlsym(RTLD_DEFAULT, "g_object_unref"));
  void* applied_ = nullptr;
  std::vector<IconBitmap> images_;
public:
  std::string last_error;
  static int scale(void* window) {
    using Scale = int (*)(void*);
    static auto get = reinterpret_cast<Scale>(dlsym(RTLD_DEFAULT, "gdk_window_get_scale_factor"));
    return window && get ? std::clamp(get(window), 1, 8) : 1;
  }
  void apply(void* window) {
    if (!window) { applied_ = nullptr; return; }
    if (window == applied_ || images_.empty()) return;
    if (!create_ || !set_ || !unref_) throw Error("HOST_UNAVAILABLE", "REAPER's GTK backend does not expose window icons");
    std::vector<List> list(images_.size());
    struct Release { std::vector<List>& list; Unref unref; ~Release() { for (auto& item : list) if (item.data) unref(item.data); } } release{list, unref_};
    for (size_t i = 0; i < images_.size(); ++i) {
      const auto& image = images_[i];
      auto bytes = static_cast<unsigned char*>(std::malloc(image.rgba.size()));
      if (!bytes) throw Error("ICON_APPLY_FAILED", "Cannot allocate window icon pixels");
      std::memcpy(bytes, image.rgba.data(), image.rgba.size());
      auto pixbuf = create_(bytes, 0, 1, 8, image.size, image.size, image.size * 4,
        +[](unsigned char* data, void*) { std::free(data); }, nullptr);
      if (!pixbuf) { std::free(bytes); throw Error("ICON_APPLY_FAILED", "Cannot create window icon pixels"); }
      list[i] = {pixbuf, i + 1 < list.size() ? &list[i+1] : nullptr, i ? &list[i-1] : nullptr};
    }
    set_(window, list.data()); applied_ = window; last_error.clear();
  }
  void set(void* window, const std::vector<IconBitmap>& images) {
    if (!create_ || !set_ || !unref_) throw Error("HOST_UNAVAILABLE", "REAPER's GTK backend does not expose window icons");
    auto previous = images_; auto previous_window = applied_;
    images_ = images; applied_ = nullptr;
    try { apply(window); last_error.clear(); } catch (...) { images_ = std::move(previous); applied_ = previous_window; throw; }
  }
  void refresh(void* window) {
    try { apply(window); }
    catch (const std::exception& error) { last_error = error.what(); applied_ = window; }
  }
};
}
