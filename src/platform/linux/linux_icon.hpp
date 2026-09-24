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
  using GetDecorations = int (*)(void*, unsigned*);
  using SetDecorations = void (*)(void*, unsigned);
  using Ref = void* (*)(void*);
  using GetData = void* (*)(void*, const char*);
  using SetData = void (*)(void*, const char*, void*);
  using Destroyed = int (*)(void*);
  using Atom = void* (*)(const char*, int);
  using GetProperty = int (*)(void*, void*, void*, unsigned long, unsigned long, int, void**, int*, int*, unsigned char**);
  using SetProperty = void (*)(void*, void*, void*, int, int, const unsigned char*, int);
  using DeleteProperty = void (*)(void*, void*);
  Create create_ = reinterpret_cast<Create>(dlsym(RTLD_DEFAULT, "gdk_pixbuf_new_from_data"));
  Set set_ = reinterpret_cast<Set>(dlsym(RTLD_DEFAULT, "gdk_window_set_icon_list"));
  Unref unref_ = reinterpret_cast<Unref>(dlsym(RTLD_DEFAULT, "g_object_unref"));
  GetDecorations get_decorations_ = reinterpret_cast<GetDecorations>(dlsym(RTLD_DEFAULT, "gdk_window_get_decorations"));
  SetDecorations set_decorations_ = reinterpret_cast<SetDecorations>(dlsym(RTLD_DEFAULT, "gdk_window_set_decorations"));
  Ref ref_ = reinterpret_cast<Ref>(dlsym(RTLD_DEFAULT, "g_object_ref"));
  GetData get_data_ = reinterpret_cast<GetData>(dlsym(RTLD_DEFAULT, "g_object_get_data"));
  SetData set_data_ = reinterpret_cast<SetData>(dlsym(RTLD_DEFAULT, "g_object_set_data"));
  Destroyed destroyed_ = reinterpret_cast<Destroyed>(dlsym(RTLD_DEFAULT, "gdk_window_is_destroyed"));
  Atom atom_ = reinterpret_cast<Atom>(dlsym(RTLD_DEFAULT, "gdk_atom_intern"));
  GetProperty get_property_ = reinterpret_cast<GetProperty>(dlsym(RTLD_DEFAULT, "gdk_property_get"));
  SetProperty set_property_ = reinterpret_cast<SetProperty>(dlsym(RTLD_DEFAULT, "gdk_property_change"));
  DeleteProperty delete_property_ = reinterpret_cast<DeleteProperty>(dlsym(RTLD_DEFAULT, "gdk_property_delete"));
  Unref free_ = reinterpret_cast<Unref>(dlsym(RTLD_DEFAULT, "g_free"));
  void* applied_ = nullptr;
  std::vector<IconBitmap> images_;
  bool visible_ = true, initialized_ = false;
  struct Property { void* atom = nullptr; void* type = nullptr; int format = 0; std::vector<unsigned char> bytes; };
  void* dock_ = nullptr;
  Property dock_icons_, dock_decorations_;
  static constexpr auto dock_owner = "ReaWebAPI.DockIcon";
  Property capture(void* window, const char* name) {
    Property value; value.atom = atom_(name, false);
    int length = 0; unsigned char* data = nullptr;
    get_property_(window, value.atom, nullptr, 0, 0x7ffffffc, false, &value.type, &value.format, &length, &data);
    std::unique_ptr<unsigned char, Unref> release(data, free_);
    if (data && length > 0) value.bytes.assign(data, data + length);
    return value;
  }
  void restore(void* window, const Property& value) {
    if (!value.type) { delete_property_(window, value.atom); return; }
    // GDK stores format-32 properties in native longs, including on 64-bit hosts.
    const auto stride = value.format == 32 ? sizeof(long) : size_t(value.format / 8);
    if (stride) set_property_(window, value.atom, value.type, value.format, 0, value.bytes.data(), int(value.bytes.size() / stride));
  }
  void release_dock() {
    auto previous = dock_; dock_ = nullptr;
    if (!previous) return;
    if (get_data_(previous, dock_owner) == this) {
      set_data_(previous, dock_owner, nullptr);
      if (!destroyed_(previous)) { restore(previous, dock_icons_); restore(previous, dock_decorations_); }
    }
    unref_(previous); applied_ = nullptr;
  }
  void sync_dock(void* window) {
    if (dock_ != window || (dock_ && get_data_(dock_, dock_owner) != this)) release_dock();
    if (!window || dock_) return;
    if (!ref_ || !unref_ || !get_data_ || !set_data_ || !destroyed_ || !atom_ || !get_property_ || !set_property_ || !delete_property_ || !free_)
      throw Error("HOST_UNAVAILABLE", "REAPER's GTK backend does not expose Docker icon state");
    if (auto previous = static_cast<LinuxIcon*>(get_data_(window, dock_owner))) previous->release_dock();
    auto icons = capture(window, "_NET_WM_ICON"), decorations = capture(window, "_MOTIF_WM_HINTS");
    dock_ = ref_(window); dock_icons_ = std::move(icons); dock_decorations_ = std::move(decorations);
    set_data_(dock_, dock_owner, this); applied_ = nullptr;
  }
public:
  ~LinuxIcon() { release_dock(); }
  std::string last_error;
  void clear(void* window, bool shared = false) {
    if (window && !set_) throw Error("HOST_UNAVAILABLE", "REAPER's GTK backend does not expose window icons");
    images_.clear(); initialized_ = true; applied_ = nullptr; apply(window, shared); last_error.clear();
  }
  static int scale(void* window) {
    using Scale = int (*)(void*);
    static auto get = reinterpret_cast<Scale>(dlsym(RTLD_DEFAULT, "gdk_window_get_scale_factor"));
    return window && get ? std::clamp(get(window), 1, 8) : 1;
  }
  void apply(void* window, bool shared = false) {
    sync_dock(shared && (!visible_ || !images_.empty()) ? window : nullptr);
    if (shared && !dock_) return;
    if (!window) { applied_ = nullptr; return; }
    if (!initialized_ && visible_) return;
    if (get_decorations_ && set_decorations_) {
      constexpr unsigned all = 1, menu = 16;
      unsigned decorations = all;
      get_decorations_(window, &decorations);
      const auto next = (visible_ == bool(decorations & all)) ? decorations & ~menu : decorations | menu;
      if (next != decorations) set_decorations_(window, next);
    }
    if (window == applied_) return;
    if (!visible_ || images_.empty()) {
      if (!set_) throw Error("HOST_UNAVAILABLE", "REAPER's GTK backend does not expose window icons");
      set_(window, nullptr); applied_ = window; last_error.clear(); return;
    }
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
  void set(void* window, const std::vector<IconBitmap>& images, bool shared = false) {
    if (!create_ || !set_ || !unref_) throw Error("HOST_UNAVAILABLE", "REAPER's GTK backend does not expose window icons");
    auto previous = images_; auto previous_window = applied_; const auto initialized = initialized_;
    images_ = images; initialized_ = true; applied_ = nullptr;
    try { apply(window, shared); last_error.clear(); }
    catch (...) { images_ = std::move(previous); applied_ = previous_window; initialized_ = initialized; throw; }
  }
  void set_visible(void* window, bool visible, bool shared = false) {
    if (!get_decorations_ || !set_decorations_) throw Error("HOST_UNAVAILABLE", "REAPER's GTK backend does not expose window decorations");
    const auto previous = visible_, initialized = initialized_; auto previous_window = applied_;
    if (visible_ != visible) { visible_ = visible; initialized_ = true; applied_ = nullptr; }
    try { apply(window, shared); }
    catch (...) { visible_ = previous; initialized_ = initialized; applied_ = previous_window; throw; }
  }
  void refresh(void* window, bool shared = false) {
    try { apply(window, shared); }
    catch (const std::exception& error) { last_error = error.what(); applied_ = window; }
  }
};
}
