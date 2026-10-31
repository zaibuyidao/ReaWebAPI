#include "platform/shared/swell_window.hpp"
#include "platform/linux/linux_channel.hpp"
#include "platform/shared/devtools.hpp"
#include "platform/shared/window_menu.hpp"
#include "platform/linux/linux_icon.hpp"
#include "platform/linux/linux_alpha.hpp"
#include "platform/linux/linux_corners.hpp"
#include "platform/linux/linux_geometry.hpp"
#include "platform/shared/transparency.hpp"
#include "platform/shared/idle_pool.hpp"
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <signal.h>
#include <spawn.h>
#include <set>
#include <sys/wait.h>
#include <thread>
#include <vector>

extern char** environ;
namespace reaweb {
namespace {
fs::path helper_path() {
  Dl_info module{};
  if (!dladdr(reinterpret_cast<void*>(&helper_path), &module) || !module.dli_fname)
    throw std::runtime_error("Cannot locate the ReaWebAPI extension directory");
  auto path = fs::path(module.dli_fname).parent_path() / REAWEB_HELPER_NAME;
  if (!fs::is_regular_file(path)) throw std::runtime_error("Missing WebKit helper beside the extension: " + path.string());
  // ReaPack and HTTP downloads may not retain the executable bit.
  if (access(path.c_str(), X_OK) != 0)
    fs::permissions(path, fs::perms::owner_exec, fs::perm_options::add);
  return path;
}
class LinuxProcess {
  pid_t pid_ = -1;
  unsigned input_source_ = 0;
  using RemoveSource = int (*)(unsigned);
  RemoveSource remove_source_ = reinterpret_cast<RemoveSource>(dlsym(RTLD_DEFAULT, "g_source_remove"));
  std::unique_ptr<LinuxChannel> channel_;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  bool ready_ = false;
  std::set<int> parked_;
  uint64_t next_desktop_ = 0;
  struct Desktop { Platform::DesktopReply reply; std::chrono::steady_clock::time_point deadline; };
  std::map<std::string, Desktop> desktop_;
public:
  std::string error;
  std::string version;
  bool frameless_supported = false;
  Json idle_state;
  std::map<int, bool> prewarmed;
  std::map<int, unsigned long> alpha_frames;
  std::map<int, WindowOptions> listeners;
  std::map<int, Json> inspectors;
  std::map<int, unsigned> backgrounds;
  explicit LinuxProcess(const fs::path& data) {
    if (!getenv("DISPLAY")) throw std::runtime_error("ReaWebAPI requires an X11 or XWayland display on Linux");
    auto executable = helper_path().string();
    auto profile = data.string();
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) throw std::runtime_error("Cannot create WebKit socketpair");
    try { channel_ = std::make_unique<LinuxChannel>(sockets[0]); }
    catch (...) { ::close(sockets[1]); throw; }
    // Keep the source above fd 3 so dup2 clears its close-on-exec flag.
    int source = fcntl(sockets[1], F_DUPFD_CLOEXEC, 10);
    ::close(sockets[1]);
    if (source < 0) throw std::runtime_error("Cannot duplicate WebKit socket");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, source, 3);
    posix_spawn_file_actions_addclose(&actions, source);
    std::vector<std::string> environment;
    for (char** item = environ; *item; ++item) if (strncmp(*item, "GDK_BACKEND=", 12)) environment.emplace_back(*item);
    environment.emplace_back("GDK_BACKEND=x11");
    std::vector<char*> env;
    for (auto& item : environment) env.push_back(item.data());
    env.push_back(nullptr);
    char* argv[] = {executable.data(), profile.data(), nullptr};
    const int result = posix_spawn(&pid_, executable.c_str(), &actions, nullptr, argv, env.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(source);
    if (result) throw std::runtime_error("Cannot start WebKit process: " + std::string(strerror(result)));
    // Feed the bridge from REAPER's existing GLib loop as soon as IPC arrives.
    // The platform timer still covers buffered input and housekeeping.
    using AddSource = unsigned (*)(int, int, unsigned, int (*)(int, unsigned, void*), void*, void (*)(void*));
    const auto add_source = reinterpret_cast<AddSource>(dlsym(RTLD_DEFAULT, "g_unix_fd_add_full"));
    if (add_source && remove_source_) input_source_ = add_source(0, channel_->fd(), 1 | 8 | 16 | 32,
      +[](int, unsigned, void* data) -> int {
        auto self = static_cast<LinuxProcess*>(data);
        self->pump();
        if (self->error.empty()) return 1;
        self->input_source_ = 0;
        return 0;
      }, this, nullptr);
  }
  ~LinuxProcess() {
    if (input_source_) remove_source_(input_source_);
    channel_.reset();
    if (pid_ > 0) {
      for (int n = 0; n < 25; ++n) {
        const auto result = waitpid(pid_, nullptr, WNOHANG);
        if (result == pid_ || (result < 0 && errno == ECHILD)) { pid_ = -1; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (pid_ > 0) { kill(pid_, SIGKILL); while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {} }
    }
  }
  void send(const Json& message) {
    if (!error.empty()) throw std::runtime_error(error);
    channel_->send(message);
  }
  void pump() {
    if (!error.empty()) return;
    try {
      channel_->pump([this](const Json& message) {
        const auto op = message.at("op").get<std::string>();
        if (op == "idle-pool") { idle_state = message.at("state"); return; }
        if (op == "desktop-result") {
          auto it = desktop_.find(message.at("request").get<std::string>());
          if (it != desktop_.end()) { auto reply = std::move(it->second.reply); desktop_.erase(it); reply(message.at("response")); }
          return;
        }
        if (op == "ready") {
          if (message.value("protocol", 0) != 1 || message.value("version", "") != REAWEB_VERSION)
            throw std::runtime_error("WebKit helper version does not match the extension. Install both files from the same release");
          version = message.value("browserVersion", "");
          frameless_supported = message.value("frameless", false); ready_ = true; return;
        }
        if (op == "parked") { parked_.insert(message.at("id").get<int>()); return; }
        auto it = listeners.find(message.at("id").get<int>());
        if (it == listeners.end()) return;
        if (op == "container") { prewarmed[it->first] = message.at("prewarmed").get<bool>(); return; }
        if (op == "loaded") {
          if (it->second.startup_timing) it->second.startup_timing->mark(StartupTiming::Loaded);
          return;
        }
        if (op == "navigating" && it->second.startup_timing) it->second.startup_timing->mark(StartupTiming::ControllerReady);
        if (op == "alpha-frame") { alpha_frames[it->first] = message.at("parent").get<unsigned long>(); return; }
        if (op == "background") { backgrounds[it->first] = message.at("color").get<unsigned>(); return; }
        if (op == "devtools-state") { inspectors[it->first] = message.at("state"); return; }
        if (op == "message") it->second.on_message(message.at("message").get<std::string>());
        else if (op == "dock-toggle" && it->second.on_dock_toggle) it->second.on_dock_toggle();
        else if (op == "drop" && it->second.on_drop) it->second.on_drop(message.at("payload"));
        else if (op == "error") it->second.on_error(message.at("error").get<std::string>());
        else if (op == "navigating" && it->second.on_navigation) it->second.on_navigation();
        else if (op == "reload-request") {
          if (!it->second.on_reload || !it->second.on_reload()) send({{"id", message.at("id")}, {"op", "reload"}});
        } else if (op == "window-menu") {
          const int command = message.at("command");
          if (command == 2) {
            if (!it->second.on_reload || !it->second.on_reload()) send({{"id", message.at("id")}, {"op", "reload"}});
          } else if (command == 5) ShellExecute(nullptr, "open", "explorer.exe", it->second.entry.parent_path().c_str(), nullptr, SW_SHOWNORMAL);
          else if (command == 6 && it->second.on_close) it->second.on_close();
        }
      });
      for (auto it = desktop_.begin(); it != desktop_.end();) {
        if (std::chrono::steady_clock::now() < it->second.deadline) { ++it; continue; }
        auto reply = std::move(it->second.reply); it = desktop_.erase(it);
        reply({{"error", {{"code", "HOST_TIMEOUT"}, {"message", "Desktop operation timed out"}}}});
      }
      if (!ready_ && std::chrono::steady_clock::now() - started_ > std::chrono::seconds(15))
        throw std::runtime_error("WebKit process did not start within 15 seconds");
    } catch (const std::exception& failure) {
      error = std::string(failure.what()) + ". Check WebKitGTK 4.1 and the X11 display, then reopen the tool.";
      for (const auto& item : listeners) item.second.on_error(error);
    }
  }
  void desktop(const std::string& method, const Json& args, Platform::DesktopReply reply, int window = 0) {
    if (desktop_.size() >= 64) throw Error("QUEUE_LIMIT", "Too many desktop operations");
    const auto id = std::to_string(++next_desktop_);
    desktop_.emplace(id, Desktop{std::move(reply), method == "ReaWeb_Drag" ? std::chrono::steady_clock::time_point::max() : std::chrono::steady_clock::now() + std::chrono::seconds(10)});
    try { send({{"id", window}, {"op", "desktop"}, {"request", id}, {"method", method}, {"args", args}}); }
    catch (...) { desktop_.erase(id); throw; }
  }
  void park(int id) {
    parked_.erase(id);
    send({{"id", id}, {"op", "park"}});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (error.empty() && !parked_.count(id) && std::chrono::steady_clock::now() < deadline) {
      pump();
      if (!parked_.count(id)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!parked_.erase(id)) throw Error("DOCK_BUSY", "WebKit is busy. Retry docking when the page is ready.");
  }
  void require_frameless() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!ready_ && error.empty() && std::chrono::steady_clock::now() < deadline) {
      pump();
      if (!ready_) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!ready_ || !error.empty() || !frameless_supported)
      throw Error("HOST_UNAVAILABLE", error.empty() ? "Frameless windows require an X11 window manager with _NET_WM_MOVERESIZE support" : error);
  }
};
class LinuxWindow final : public Window, public std::enable_shared_from_this<LinuxWindow> {
  std::shared_ptr<LinuxProcess> process_;
  int id_;
  std::unique_ptr<SwellWindow> window_;
  Json geometry_;
  mutable Json normal_;
  mutable LinuxGeometry native_geometry_;
  mutable uint64_t placement_revision_ = 0;
  uint64_t origin_revision_ = 0;
  int native_x_ = 0, native_y_ = 0;
  bool maximized_ = false;
  LinuxIcon icon_;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point last_resize_sync_{};
  bool immediate_resize_enabled_ = false;
  bool pending_origin_valid_ = false;
  bool focus_pending_ = false;
  int pending_origin_x_ = 0, pending_origin_y_ = 0;
  LONG_PTR saved_style_ = 0;
  bool style_saved_ = false;
  bool initial_hidden_ = false, cancelled_ = false, wants_visible_ = true;
  bool frameless_ = false, resizable_ = true;
  bool transparent_ = false;
  LinuxAlpha alpha_;
  LinuxCorners corners_;
  double corner_radius_ = 0;
  void* frame_window_ = nullptr;
  bool frame_override_ = false;
  void update_frame() {
    if (!frame_override_ || docked()) return;
    const auto handle = static_cast<HWND>(window_->handle());
    auto native = SWELL_GetOSWindow(handle, "GdkWindow");
    if (!native || native == frame_window_) return;
    using SetDecorations = void (*)(void*, unsigned);
    auto set = reinterpret_cast<SetDecorations>(dlsym(RTLD_DEFAULT, "gdk_window_set_decorations"));
    if (set) {
      set(native, frameless_ ? 0u : resizable_ ? 1u : 2u | 8u | 16u | 32u); frame_window_ = native;
      icon_.refresh(nullptr); icon_.refresh(native);
    }
  }
  void update_corners() {
    auto handle = static_cast<HWND>(window_->handle());
    if (GetWindowLong(handle, GWL_STYLE) & WS_CHILD) return;
    RECT rect{}; GetClientRect(handle, &rect);
    corners_.apply(SWELL_GetOSWindow(handle, "GdkWindow"), rect.right, rect.bottom, corner_radius_active() ? corner_radius_ : 0);
  }
public:
  bool dock_reparents_window() const override { return true; }
  unsigned native_state() const {
    using State = unsigned (*)(void*);
    static auto state = reinterpret_cast<State>(dlsym(RTLD_DEFAULT, "gdk_window_get_state"));
    auto native = SWELL_GetOSWindow(static_cast<HWND>(window_->handle()), "GdkWindow");
    return state && native ? state(native) : 0;
  }
  bool sync_resize() {
    if (closed() || !immediate_resize_enabled_ || !geometry_.is_object()) return false;
    auto handle = static_cast<HWND>(window_->handle());
    RECT rect{};
    GetClientRect(handle, &rect);
    const int width = std::max(1, int(rect.right - rect.left));
    const int height = std::max(1, int(rect.bottom - rect.top));
    if (geometry_.value("width", 0) == width && geometry_.value("height", 0) == height) return false;
    process_->send({{"id", id_}, {"op", "resize"}, {"width", width}, {"height", height}});
    geometry_["width"] = width;
    geometry_["height"] = height;
    return true;
  }
  LinuxWindow(std::shared_ptr<LinuxProcess> process, int id, WindowOptions options) : process_(std::move(process)), id_(id), initial_hidden_(options.hidden), wants_visible_(!options.hidden) {
    if (options.startup_timing) options.startup_timing->mark(StartupTiming::ControllerStart);
    frameless_ = options.frameless; resizable_ = options.resizable;
    transparent_ = options.transparent;
    icon_.decorations = !frameless_;
    auto on_close = std::move(options.on_close);
    options.on_close = [this, on_close = std::move(on_close)]() mutable {
      immediate_resize_enabled_ = false;
      if (on_close) on_close();
    };
    window_ = std::make_unique<SwellWindow>(options.title, options.parent, [this] {
      focus_pending_ = true;
    }, options.on_close, /*hidden=*/true, options.hosted);
    if (options.hosted) window_->before_destroy = [this] {
      immediate_resize_enabled_ = false; wants_visible_ = false;
      geometry_ = Json(); pending_origin_valid_ = false;
      try { process_->park(id_); } catch (...) {}
    };
    if (transparent_) {
      SetProp(static_cast<HWND>(window_->handle()), "SWELLGdkAlphaChannel", reinterpret_cast<HANDLE>(1));
      window_->set_transparent(true);
    }
    if (frameless_ || !resizable_) {
      const auto handle = static_cast<HWND>(window_->handle());
      auto style = GetWindowLong(handle, GWL_STYLE);
      if (frameless_) style = style & ~(WS_CAPTION | WS_BORDER);
      if (!resizable_) style &= ~WS_THICKFRAME;
      SetWindowLong(handle, GWL_STYLE, style);
    }
    process_->send({{"id", id_}, {"op", "open"}, {"uri", options.url.empty() ? file_uri(options.entry) : options.url},
      {"script", options.script}, {"lifecycleReload", static_cast<bool>(options.on_reload)}, {"dockEnabled", static_cast<bool>(options.on_dock_toggle)},
      {"appName", options.app_name ? options.app_name() : options.title},
      {"frameless", frameless_}, {"resizable", resizable_}, {"transparent", transparent_}, {"hosted", options.hosted},
      {"resourceRoot", options.resources ? options.resources->root().u8string() : ""}});
    process_->listeners.emplace(id_, std::move(options));
    process_->backgrounds.emplace(id_, 0xffffff);
    // A floating window must mirror the native resize without waiting for the
    // next platform timer tick, so sync it here and let the timer cover the rest.
    window_->resize = [this] {
      native_geometry_.invalidate();
      try {
        update_corners();
        // GDK writes the _NET_WM_SYNC_REQUEST acknowledgement counter back to the
        // window manager only while it actually paints a frame
        // (gdk_x11_window_end_frame in gdkwindow-x11.c). When the window shrinks,
        // the new size already contains old content, so the X server sends no
        // Expose, this process paints nothing, the window manager receives no
        // acknowledgement, and it allows the next resize step only after its full
        // 1 second timeout, which freezes the border. Request the repaint here.
        //
        // Only a floating window is a top-level window managed directly by the
        // window manager, and only it is bound by the sync counter acknowledgement.
        // While docked this window is a child of REAPER and the Docker schedules
        // its repaints, so do not interfere there.
        const auto handle = static_cast<HWND>(window_->handle());
        if (!(GetWindowLong(handle, GWL_STYLE) & WS_CHILD)) InvalidateRect(handle, nullptr, FALSE);
        if (closed() || !immediate_resize_enabled_) return;
        if (native_state() & 2) { immediate_resize_enabled_ = false; return; }
        const auto now = std::chrono::steady_clock::now();
        if (last_resize_sync_.time_since_epoch().count() &&
            now - last_resize_sync_ < std::chrono::milliseconds(8)) return;
        if (sync_resize()) last_resize_sync_ = now;
      } catch (const std::exception& error) {
        process_->listeners.at(id_).on_error(error.what());
      }
    };
    window_->context_menu = [this](LPARAM position) {
      auto keep_alive = shared_from_this();
      try {
        process_->pump();
        const auto options = process_->listeners.at(id_);
        const auto it = process_->inspectors.find(id_);
        DevToolsMenuState state;
        if (it != process_->inspectors.end()) {
          state.shown = it->second.value("visible", false);
          state.floating = it->second.value("mode", "embedded") == "floating";
        }
        show_window_menu(static_cast<HWND>(window_->handle()), position, options, state, [this](DevToolsAction action) {
          process_->send({{"id", id_}, {"op", "devtools-action"}, {"action", static_cast<int>(action)}});
        }, [this] { reload(); });
      } catch (const std::exception& error) { process_->listeners.at(id_).on_error(error.what()); }
    };
  }
  ~LinuxWindow() override {
    native_geometry_.watch(nullptr);
    alpha_.detach();
    process_->alpha_frames.erase(id_);
    immediate_resize_enabled_ = false;
    window_->resize = {};
    window_->before_destroy = {};
    process_->listeners.erase(id_);
    process_->prewarmed.erase(id_);
    process_->inspectors.erase(id_);
    process_->backgrounds.erase(id_);
    try {
      // Detach before SWELL destroys the X11 parent, preserving other pages.
      process_->park(id_);
      process_->send({{"id", id_}, {"op", "close"}});
    } catch (...) {}
  }
  void sync() {
    if (closed()) {
      immediate_resize_enabled_ = false;
      return;
    }
    if (!transparent_) window_->set_background(process_->backgrounds.at(id_));
    else {
      const auto reason = LinuxAlpha::unavailable();
      if (!reason.empty()) { process_->listeners.at(id_).on_error("TRANSPARENCY_UNAVAILABLE: " + reason); return; }
      auto frame = process_->alpha_frames.find(id_);
      if (frame != process_->alpha_frames.end()) alpha_.present(frame->second);
      if (alpha_.pending() && std::chrono::steady_clock::now() - started_ > std::chrono::seconds(15)) {
        process_->listeners.at(id_).on_error("TRANSPARENCY_FAILED: WebKitGTK did not prepare the initial Alpha frame"); return;
      }
    }
    const bool is_docked = docked();
    update_frame();
    update_corners();
    if (!process_->listeners.at(id_).hosted) icon_.refresh(icon_target(is_docked), is_docked);
    if (!wants_visible_) { immediate_resize_enabled_ = false; return; }
    using GetXid = unsigned long (*)(void*);
    static auto get_xid = reinterpret_cast<GetXid>(dlsym(RTLD_DEFAULT, "gdk_x11_window_get_xid"));
    if (!get_xid) get_xid = reinterpret_cast<GetXid>(dlsym(RTLD_DEFAULT, "gdk_x11_drawable_get_xid"));
    auto handle = static_cast<HWND>(window_->handle());
    HWND ancestor = handle;
    void* native = nullptr;
    while (ancestor) {
      native = SWELL_GetOSWindow(ancestor, "GdkWindow");
      if (native) break;
      ancestor = GetParent(ancestor);
    }
    if (!get_xid || !native) {
      if (std::chrono::steady_clock::now() - started_ > std::chrono::seconds(5))
        process_->listeners.at(id_).on_error("REAPER did not expose an X11 window for WebKit embedding");
      return;
    }
    RECT rect{};
    GetClientRect(handle, &rect);
    POINT origin{0, 0};
    ClientToScreen(handle, &origin);
    // X11 embedding is relative to the GDK window, including REAPER's menu area.
    using GetOrigin = int (*)(void*, int*, int*);
    static auto get_origin = reinterpret_cast<GetOrigin>(dlsym(RTLD_DEFAULT, "gdk_window_get_origin"));
    const auto revision = native_geometry_.watch(native);
    if (revision != origin_revision_ && get_origin && get_origin(native, &native_x_, &native_y_)) origin_revision_ = revision;
    if (origin_revision_ == revision) { origin.x -= native_x_; origin.y -= native_y_; }
    else ScreenToClient(ancestor, &origin);
    const auto& options = process_->listeners.at(id_);
    const int raw_x = origin.x, raw_y = origin.y;
    const int next_width = rect.right - rect.left;
    const int next_height = std::max(1, int(rect.bottom - rect.top));
    const bool next_visible = transparent_ ? window_->visible() && !(native_state() & 2) : visible();
    const bool next_focused = focused();
    Json next = {{"id", id_}, {"op", "geometry"}, {"parent", get_xid(native)}, {"x", raw_x}, {"y", raw_y},
      {"width", next_width}, {"height", next_height}, {"visible", next_visible}, {"focused", next_focused},
      {"docked", is_docked}, {"appName", options.app_name ? options.app_name() : options.title}};
    const bool has_geometry = geometry_.is_object();
    const auto old_width = has_geometry ? geometry_.value("width", 0) : 0;
    const auto old_height = has_geometry ? geometry_.value("height", 0) : 0;
    const auto old_x = has_geometry ? geometry_.value("x", 0) : raw_x;
    const auto old_y = has_geometry ? geometry_.value("y", 0) : raw_y;
    const auto parent_id = next.at("parent").get<unsigned long>();
    const auto next_focused_value = next.at("focused").get<bool>();
    const auto next_docked = next.at("docked").get<bool>();
    const auto next_app_name = next.at("appName").get<std::string>();
    const bool non_origin_changed = !has_geometry ||
      geometry_.value("parent", 0UL) != parent_id ||
      old_width != next_width || old_height != next_height ||
      geometry_.value("visible", false) != next_visible ||
      geometry_.value("docked", false) != next_docked ||
      geometry_.value("appName", std::string()) != next_app_name;
    if (!has_geometry || non_origin_changed || (old_x == raw_x && old_y == raw_y)) {
      pending_origin_valid_ = false;
    } else if (pending_origin_valid_ && pending_origin_x_ == raw_x && pending_origin_y_ == raw_y) {
      pending_origin_valid_ = false;
    } else {
      pending_origin_x_ = raw_x;
      pending_origin_y_ = raw_y;
      pending_origin_valid_ = true;
      next["x"] = old_x;
      next["y"] = old_y;
    }
    const auto next_x = next.at("x").get<int>();
    const auto next_y = next.at("y").get<int>();
    const bool layout_changed = !has_geometry ||
      geometry_.value("parent", 0UL) != parent_id ||
      geometry_.value("x", 0) != next_x || geometry_.value("y", 0) != next_y ||
      geometry_.value("width", 0) != next_width || geometry_.value("height", 0) != next_height ||
      geometry_.value("visible", false) != next_visible ||
      geometry_.value("docked", false) != next_docked ||
      geometry_.value("appName", std::string()) != next_app_name;
    const bool focus_changed = !has_geometry ||
      geometry_.value("focused", false) != next_focused_value;
    if (layout_changed) {
      process_->send(next);
      geometry_ = next;
    }
    // Send focus after SWELL activation and the helper's current embedding geometry.
    if (focus_changed || focus_pending_) {
      process_->send({{"id", id_}, {"op", "focus"}, {"focused", next_focused_value}});
      geometry_["focused"] = next_focused_value;
      focus_pending_ = false;
    }
    immediate_resize_enabled_ = !next_docked && next_visible;
  }
  void evaluate(const std::string& script) override { process_->send({{"id", id_}, {"op", "eval"}, {"script", script}}); }
  void devtools() override { process_->send({{"id", id_}, {"op", "devtools"}}); }
  Json devtools_state() const override {
    DevToolsPreferences prefs;
    auto it = process_->inspectors.find(id_);
    if (it != process_->inspectors.end()) prefs.restore(it->second);
    return prefs.state();
  }
  void restore_devtools(const Json& value) override {
    DevToolsPreferences prefs; prefs.restore(value);
    process_->inspectors[id_] = prefs.state();
    process_->send({{"id", id_}, {"op", "devtools-restore"}, {"state", prefs.state()}});
  }
  bool closed() const override { return window_->closed() || !process_->error.empty(); }
  void* native_handle() const override { return window_->handle(); }
  unsigned background_color() const override { return process_->backgrounds.at(id_); }
  void reparent_host(void* parent) override {
    auto& options = process_->listeners.at(id_);
    if (!options.hosted) throw Error("HOST_UNAVAILABLE", "Expected a hosted window");
    set_visible(false);
    window_->reparent_host(parent);
    options.parent = window_->host_parent();
    geometry_ = Json(); pending_origin_valid_ = false;
  }
  void prepare_dock() override {
    if (transparent_) throw Error("DOCK_UNAVAILABLE", "Transparent windows require a standalone floating host");
    native_geometry_.invalidate();
    corners_.reset(SWELL_GetOSWindow(static_cast<HWND>(window_->handle()), "GdkWindow"));
    frame_window_ = nullptr;
    immediate_resize_enabled_ = false;
    last_resize_sync_ = {};
    normal_ = placement(); maximized_ = (native_state() & 4) != 0;
    if (maximized_) ShowWindow(static_cast<HWND>(window_->handle()), SW_RESTORE);
    window_->prepare_dock();
    // REAPER's GTK Docker reparents this window, and SWELL's GDK backend only
    // turns a reparented window into a child window when it already carries the
    // child style. Hide it first so the style change cannot flash on screen.
    const auto handle = static_cast<HWND>(window_->handle());
    if (!style_saved_) {
      saved_style_ = GetWindowLong(handle, GWL_STYLE);
      saved_style_ = frameless_ ? saved_style_ & ~(WS_CAPTION | WS_BORDER) : saved_style_ | WS_CAPTION | WS_BORDER;
      style_saved_ = true;
    }
    ShowWindow(handle, SW_HIDE);
    SetWindowLong(handle, GWL_STYLE, WS_CHILD);
    prepare_undock();
  }
  void prepare_undock() override {
    immediate_resize_enabled_ = false;
    window_->set_visible(false);
    icon_.refresh(nullptr);
    last_resize_sync_ = {};
    if (!geometry_.is_null()) {
      // Invalidate even on timeout so the next pump can reattach a late acknowledgement.
      geometry_ = Json();
      pending_origin_valid_ = false;
      process_->park(id_);
    }
  }
  void restore_floating() override {
    immediate_resize_enabled_ = false;
    last_resize_sync_ = {};
    icon_.refresh(nullptr);
    if (style_saved_) {
      SetWindowLong(static_cast<HWND>(window_->handle()), GWL_STYLE, saved_style_);
      style_saved_ = false;
    }
    window_->restore_floating();
    if (!normal_.is_null()) restore_placement(normal_);
    if (maximized_ && !initial_hidden_) ShowWindow(static_cast<HWND>(window_->handle()), SW_SHOWMAXIMIZED);
  }
  void focus() override { if (!cancelled_) { show(); window_->focus(); } }
  void cancel_pending_show() override { cancelled_ = true; }
  void show() override {
    if (cancelled_) return;
    const bool restore_maximized = initial_hidden_ && maximized_;
    initial_hidden_ = false; set_visible(true);
    if (restore_maximized) ShowWindow(static_cast<HWND>(window_->handle()), SW_SHOWMAXIMIZED);
  }
  void without_focus(const std::function<void()>& action) override {
    const auto foreground = GetForegroundWindow(), focus = GetFocus();
    const auto restore = [&] {
      if (foreground && IsWindow(foreground) && GetForegroundWindow() != foreground) SetForegroundWindow(foreground);
      if (focus && IsWindow(focus) && GetFocus() != focus) SetFocus(focus);
    };
    try { action(); } catch (...) { restore(); throw; }
    restore();
  }
  void set_title(const std::string& title) override { window_->set_title(title); }
  std::vector<int> icon_sizes() const override {
    const auto scale = LinuxIcon::scale(SWELL_GetOSWindow(static_cast<HWND>(window_->handle()), "GdkWindow"));
    return {16 * scale, 32 * scale};
  }
  void set_icon(const std::vector<IconBitmap>& images) override {
    icon_.set(icon_target(), images, docked());
  }
  bool docked() const {
    const auto& options = process_->listeners.at(id_);
    return options.is_docked && options.is_docked();
  }
  void* icon_target(bool is_docked) const {
    auto handle = static_cast<HWND>(window_->handle());
    if (!is_docked) return SWELL_GetOSWindow(handle, "GdkWindow");
    if (!window_->visible()) return nullptr;
    const auto main = process_->listeners.at(id_).parent;
    while (handle && handle != main) {
      if (auto native = SWELL_GetOSWindow(handle, "GdkWindow")) return native;
      handle = GetParent(handle);
    }
    return nullptr;
  }
  void* icon_target() const override { return icon_target(docked()); }
  void clear_icon() override { icon_.clear(icon_target(), docked()); }
  void set_icon_visible(bool visible) override { icon_.set_visible(icon_target(), visible, docked()); }
  void set_visible(bool visible) override {
    if (frame_override_ && !docked()) {
      const auto handle = static_cast<HWND>(window_->handle());
      if (!SWELL_GetOSWindow(handle, "GdkWindow")) {
        auto style = GetWindowLong(handle, GWL_STYLE);
        SetWindowLong(handle, GWL_STYLE, frameless_ ? style & ~(WS_CAPTION | WS_BORDER) : style | WS_CAPTION | WS_BORDER);
      }
    }
    const bool previous = wants_visible_;
    if (visible && !wants_visible_) started_ = std::chrono::steady_clock::now();
    wants_visible_ = visible;
    if (!visible) {
      frame_window_ = nullptr;
      corners_.reset(SWELL_GetOSWindow(static_cast<HWND>(window_->handle()), "GdkWindow"));
      immediate_resize_enabled_ = false;
      // SWELL may destroy a hidden floating X11 host. Park its foreign child first.
      if (!geometry_.is_null()) {
        geometry_ = Json(); pending_origin_valid_ = false;
        try { process_->park(id_); }
        catch (...) { wants_visible_ = previous; throw; }
      }
    }
    if (transparent_) {
      if (visible) alpha_.show(*window_);
      else { alpha_.detach(); process_->alpha_frames.erase(id_); window_->set_visible(false); }
    } else window_->set_visible(visible);
  }
  bool visibility_requested() const override { return initial_hidden_ ? wants_visible_ : visible(); }
  void show_after_create() override {
    immediate_resize_enabled_ = false;
    if (!initial_hidden_ && !cancelled_) {
      if (transparent_) set_visible(true);
      else window_->set_visible(true);
    }
  }
  Json bounds() const override { return window_->placement(); }
  void reload() override { process_->send({{"id", id_}, {"op", "reload"}}); }
  void set_drop_enabled(bool enabled) override { process_->send({{"id", id_}, {"op", "drop-enabled"}, {"enabled", enabled}}); }
  void start_drag(const Json& payload, Reply reply) override { process_->desktop("ReaWeb_Drag", payload, std::move(reply), id_); }
  void media(const std::string& method, const Json& args, Reply reply) override { process_->desktop(method, args, std::move(reply), id_); }
  void clear_media() override {
    Window::clear_media();
    if (!closed()) process_->send({{"id", id_}, {"op", "media-clear"}});
  }
  bool visible() const override { return window_->visible() && !alpha_.pending() && !(native_state() & 2); }
  bool transparent_active() const override { return transparent_ && alpha_.active() && !closed(); }
  bool focused() const override { return window_->focused(); }
  bool frameless_active() const override {
    const auto handle = static_cast<HWND>(window_->handle());
    return frameless_ && !closed() && !docked() && !(GetWindowLong(handle, GWL_STYLE) & WS_CHILD);
  }
  void set_frameless(bool value) override {
    if (value == frameless_) return;
    if (value) process_->require_frameless();
    if (!dlsym(RTLD_DEFAULT, "gdk_window_set_decorations")) throw Error("HOST_UNAVAILABLE", "Native decoration control unavailable");
    const auto change = [value](LONG_PTR style) { return value ? style & ~(WS_CAPTION | WS_BORDER) : style | WS_CAPTION | WS_BORDER; };
    process_->send({{"id", id_}, {"op", "frameless"}, {"value", value}});
    frameless_ = value;
    process_->listeners.at(id_).frameless = value;
    icon_.decorations = !value;
    frame_override_ = true; frame_window_ = nullptr;
    if (style_saved_) saved_style_ = change(saved_style_);
    if (!docked()) {
      const auto handle = static_cast<HWND>(window_->handle());
      // SWELL remaps visible hosts when WS_CAPTION changes. Set GDK decorations
      // directly to retain the mapped host, focus and embedded WebKit surface.
      if (!SWELL_GetOSWindow(handle, "GdkWindow")) SetWindowLong(handle, GWL_STYLE, change(GetWindowLong(handle, GWL_STYLE)));
      update_frame();
      update_corners(); sync_resize();
    }
  }
  void set_corner_radius(double value) override {
    if (value && !corners_.supported()) throw Error("HOST_UNAVAILABLE", "Native rounding requires GDK3/X11 window shaping");
    corner_radius_ = value; update_corners();
  }
  bool corner_radius_active() const override {
    return corner_radius_ > 0 && corners_.supported() && frameless_active() && !(native_state() & (4 | 16)) &&
      SWELL_GetOSWindow(static_cast<HWND>(window_->handle()), "GdkWindow");
  }
  Json window_capabilities() const override {
    const auto reason = LinuxAlpha::unavailable();
    return {{"hiddenStart", true}, {"frameless", process_->frameless_supported}, {"setFrameless", process_->frameless_supported},
      {"cornerRadius", {{"supported", corners_.supported()}, {"framed", false}, {"frameless", corners_.supported()}, {"max", 64}}},
      {"resizable", true}, {"dragRegions", process_->frameless_supported}, {"windowSystem", "x11"},
      {"transparent", true}, {"transparency", transparency_capabilities(reason.empty(), "x11-rgba-webkitgtk", reason, process_->frameless_supported)}};
  }
  Json placement() const override {
    if (initial_hidden_ && !normal_.is_null()) return normal_;
    const bool maximized = (native_state() & 4) != 0;
    const auto handle = static_cast<HWND>(window_->handle());
    // Child placement also depends on its SWELL ancestors. Cache only floating
    // native windows, whose move/resize and decoration changes are observed.
    const auto native = (GetWindowLong(handle, GWL_STYLE) & WS_CHILD) ? nullptr : SWELL_GetOSWindow(handle, "GdkWindow");
    const auto revision = native_geometry_.watch(native);
    if (!maximized && (cancelled_ || normal_.is_null() || placement_revision_ != revision)) {
      normal_ = window_->placement(); placement_revision_ = revision;
    }
    auto value = normal_.is_null() ? window_->placement() : normal_;
    if (!value.is_null()) value["maximized"] = maximized;
    return value;
  }
  void restore_placement(const Json& value) override {
    native_geometry_.invalidate();
    normal_ = value;
    window_->restore_placement(value);
    maximized_ = value.value("maximized", false);
    if (maximized_ && window_->visible()) ShowWindow(static_cast<HWND>(window_->handle()), SW_SHOWMAXIMIZED);
  }
  Json diagnostics() const override {
    auto inspector = devtools_state();
    auto it = process_->inspectors.find(id_);
    if (it != process_->inspectors.end()) inspector.update(it->second);
    inspector["embeddedSupported"] = true;
    const auto pooled = process_->prewarmed.find(id_);
    return {{"backend", "WebKitGTK"}, {"controllerPrewarmed", pooled != process_->prewarmed.end() && pooled->second}, {"browserVersion", process_->version}, {"helperProtocol", 1}, {"devtools", inspector}, {"iconError", icon_.last_error}, {"windowFeatures", window_capabilities()}};
  }
};
class LinuxPlatform final : public Platform {
  fs::path data_;
  std::shared_ptr<LinuxProcess> process_;
  std::vector<std::weak_ptr<LinuxWindow>> windows_;
  int next_id_ = 0;
  unsigned idle_count_ = 0;
  std::chrono::steady_clock::time_point idle_retry_{};
  void prepare_process() {
    if (process_ && (process_->error.empty() || !process_->listeners.empty())) return;
    process_ = std::make_shared<LinuxProcess>(data_);
    process_->send({{"id", 0}, {"op", "idle-pool"}, {"target", idle_count_}});
  }
public:
  explicit LinuxPlatform(fs::path data) : data_(std::move(data)) {}
  std::shared_ptr<Window> open(WindowOptions options) override {
    if (options.transparent) {
      const auto reason = LinuxAlpha::unavailable();
      if (!reason.empty()) throw Error("TRANSPARENCY_UNAVAILABLE", reason);
    }
    prepare_process();
    if (options.frameless) process_->require_frameless();
    auto window = std::make_shared<LinuxWindow>(process_, ++next_id_, std::move(options));
    windows_.push_back(window);
    return window;
  }
  void set_idle_capacity(unsigned count) override {
    if (count > max_idle_webviews) throw Error("INVALID_ARGUMENT", "Idle WebView count must be 0..32");
    idle_count_ = count;
    if (process_ && process_->error.empty()) process_->send({{"id", 0}, {"op", "idle-pool"}, {"target", count}});
  }
  Json idle_diagnostics() const override { return process_ ? process_->idle_state : Json(); }
  void idle() override {
    if (std::chrono::steady_clock::now() < idle_retry_) return;
    try {
      prepare_process();
      process_->send({{"id", 0}, {"op", "idle"}});
    } catch (const std::exception&) { idle_retry_ = std::chrono::steady_clock::now() + std::chrono::seconds(1); }
  }
  void desktop(const std::string& method, const Json& args, DesktopReply reply) override {
    if (!process_) throw Error("HOST_UNAVAILABLE", "WebKit is not running");
    process_->desktop(method, args, std::move(reply));
  }
  void pump() override {
    if (!process_) return;
    process_->pump();
    for (auto it = windows_.begin(); it != windows_.end();) {
      if (auto window = it->lock()) {
        try { window->sync(); } catch (const std::exception& error) {
          process_->error = error.what();
          for (const auto& item : process_->listeners) item.second.on_error(process_->error);
        }
        ++it;
      } else it = windows_.erase(it);
    }
  }
};
}
std::unique_ptr<Platform> make_platform(const fs::path& data) { return std::make_unique<LinuxPlatform>(data); }
}
