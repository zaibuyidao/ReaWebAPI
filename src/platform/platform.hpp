#pragma once
#include "core/core.hpp"
#include "runtime/icon.hpp"
#include <memory>

namespace reaweb {
struct DockApi {
  void* parent = nullptr;
  std::function<void(void*, const std::string&, const std::string&)> add;
  std::function<void(void*)> remove;
  std::function<int(void*)> index;
  std::function<void(void*)> activate;
  std::function<void(const std::string&, int)> remember;
  std::function<void(void*)> refresh;
};
struct WindowOptions {
  fs::path entry;
  std::string script;
  std::string title;
  std::function<void(std::string)> on_message;
  std::function<void(std::string)> on_error;
  void* parent = nullptr;
  std::function<void()> on_navigation;
  std::string url;
  std::function<void()> on_close;
  std::function<bool()> on_reload;
  std::function<void(Json)> on_drop;
  std::function<void()> on_dock_toggle;
  std::function<bool()> is_docked;
};
class Window {
public:
  using Reply = std::function<void(Json)>;
  virtual ~Window() = default;
  virtual void evaluate(const std::string& script) = 0;
  virtual void devtools() = 0;
  virtual Json devtools_state() const { return nullptr; }
  virtual void restore_devtools(const Json&) {}
  virtual bool closed() const = 0;
  virtual void* native_handle() const { return nullptr; }
  virtual void prepare_dock() {}
  virtual void prepare_undock() {}
  virtual void restore_floating() {}
  virtual void tick() {}
  virtual void focus() {}
  virtual void set_title(const std::string&) {}
  virtual std::vector<int> icon_sizes() const { return {16, 32}; }
  virtual void set_icon(const std::vector<IconBitmap>&) { throw Error("HOST_UNAVAILABLE", "Native window icons unavailable"); }
  virtual void clear_icon() { throw Error("HOST_UNAVAILABLE", "Native window icons unavailable"); }
  virtual bool visible() const { return !closed(); }
  virtual bool focused() const { return false; }
  virtual Json placement() const { return nullptr; }
  virtual void restore_placement(const Json&) {}
  virtual Json bounds() const { return placement(); }
  virtual void set_visible(bool) { throw Error("HOST_UNAVAILABLE", "Window visibility control unavailable"); }
  virtual void reload() { evaluate("window.location.reload();"); }
  virtual void set_drop_enabled(bool enabled) { if (enabled) throw Error("HOST_UNAVAILABLE", "Native drop unavailable"); }
  virtual void start_drag(const Json&, Reply) { throw Error("HOST_UNAVAILABLE", "Native drag unavailable"); }
  virtual Json diagnostics() const { return {{"backend", "unknown"}}; }
};
class Platform {
public:
  using DesktopReply = std::function<void(Json)>;
  virtual ~Platform() = default;
  virtual std::shared_ptr<Window> open(WindowOptions options) = 0;
  virtual void pump() {}
  virtual void desktop(const std::string&, const Json&, DesktopReply) { throw Error("HOST_UNAVAILABLE", "Desktop integration unavailable"); }
};
std::unique_ptr<Platform> make_platform(const fs::path& shared_data);
}
