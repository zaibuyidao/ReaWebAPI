#pragma once
#include "core/core.hpp"
#include "runtime/icon.hpp"
#include "web/web_resources.hpp"
#include "platform/shared/startup_timing.hpp"
#include "platform/shared/overlay.hpp"
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
  std::function<void(bool, bool)> daw_space;
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
  std::function<std::string()> app_name;
  std::shared_ptr<WebResources> resources;
  std::function<void()> on_present;
  bool hidden = false;
  bool frameless = false;
  bool resizable = true;
  bool transparent = false;
  bool hosted = false;
  std::shared_ptr<StartupTiming> startup_timing;
  std::function<bool()> captures_keyboard;
  OverlayOptions overlay;
};
class Window {
  std::shared_ptr<WebResources> media_resources_;
public:
  using Reply = std::function<void(Json)>;
  virtual ~Window() { if (media_resources_) media_resources_->clear_media(); }
  void set_media_resources(std::shared_ptr<WebResources> resources) { media_resources_ = std::move(resources); }
  virtual void clear_media() { if (media_resources_) media_resources_->clear_media(); }
  virtual void media(const std::string& method, const Json& args, Reply reply) {
    if (!media_resources_) throw Error("HOST_UNAVAILABLE", "Media URLs require a local App document");
    reply({{"result", method == "ReaWeb_OpenMedia" ? media_resources_->open_media(fs::u8path(args.at(0).get<std::string>())) :
      Json(media_resources_->close_media(args.at(0).get<std::string>()))}});
  }
  virtual void evaluate(const std::string& script) = 0;
  virtual void devtools() = 0;
  virtual Json devtools_state() const { return nullptr; }
  virtual void restore_devtools(const Json&) {}
  virtual bool closed() const = 0;
  virtual void* native_handle() const { return nullptr; }
  virtual void reparent_host(void*) { throw Error("HOST_UNAVAILABLE", "Hosted window reparenting unavailable"); }
  virtual unsigned background_color() const { return 0xffffff; }
  virtual void prepare_dock() {}
  virtual void prepare_undock() {}
  virtual void restore_floating() {}
  virtual void show_after_create() {}
  // True when docking reparents the native window into REAPER's Docker and hides
  // it while that happens, which costs the window its visibility and its
  // remembered Docker slot. The runtime restores both after the operation.
  // Backends that leave the native window alone return false and are unaffected.
  virtual bool dock_reparents_window() const { return false; }
  virtual void tick() {}
  virtual void focus() {}
  virtual void set_title(const std::string&) {}
  virtual std::vector<int> icon_sizes() const { return {16, 32}; }
  virtual void* icon_target() const { return native_handle(); }
  virtual void set_icon(const std::vector<IconBitmap>&) { throw Error("HOST_UNAVAILABLE", "Native window icons unavailable"); }
  virtual void clear_icon() { throw Error("HOST_UNAVAILABLE", "Native window icons unavailable"); }
  virtual void set_icon_visible(bool) {}
  virtual bool visible() const { return !closed(); }
  virtual bool visibility_requested() const { return visible(); }
  virtual bool presentation_pending() const { return false; }
  virtual void cancel_pending_show() {}
  virtual bool focused() const { return false; }
  virtual bool frameless_active() const { return false; }
  virtual void set_frameless(bool) { throw Error("HOST_UNAVAILABLE", "Runtime window style control unavailable"); }
  virtual void set_corner_radius(double) { throw Error("HOST_UNAVAILABLE", "Native window rounding unavailable"); }
  virtual bool corner_radius_active() const { return false; }
  virtual bool layer_active() const { return false; }
  virtual bool mouse_mode_active() const { return false; }
  virtual void set_mouse_mode(const std::string&) { throw Error("MOUSE_MODE_UNAVAILABLE", "Native mouse policy unavailable"); }
  virtual void set_mouse_regions(const std::vector<MouseRegion>&) { throw Error("MOUSE_MODE_UNAVAILABLE", "Native mouse regions unavailable"); }
  virtual void clear_mouse_regions() {}
  virtual bool transparent_active() const { return false; }
  virtual Json window_capabilities() const { return {{"hiddenStart", true}, {"frameless", false}, {"resizable", false},
    {"transparent", true}, {"transparency", {{"supported", false}, {"framed", false}, {"frameless", false},
      {"floatingOnly", true}, {"backend", "unknown"}, {"reason", "Native desktop Alpha is unavailable"}}}}; }
  virtual Json placement() const { return nullptr; }
  virtual void restore_placement(const Json&) {}
  virtual Json bounds() const { return placement(); }
  virtual void set_visible(bool) { throw Error("HOST_UNAVAILABLE", "Window visibility control unavailable"); }
  virtual void show() { set_visible(true); }
  virtual void without_focus(const std::function<void()>& action) { action(); }
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
  virtual void set_idle_capacity(unsigned) {}
  virtual void idle() {}
  virtual Json idle_diagnostics() const { return nullptr; }
  virtual void desktop(const std::string&, const Json&, DesktopReply) { throw Error("HOST_UNAVAILABLE", "Desktop integration unavailable"); }
};
std::unique_ptr<Platform> make_platform(const fs::path& shared_data);
#ifdef __APPLE__
bool native_key_repeat();
#endif
}
