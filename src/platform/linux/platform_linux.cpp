#include "platform/shared/swell_window.hpp"
#include "platform/linux/linux_channel.hpp"
#include "platform/shared/devtools.hpp"
#include "platform/linux/linux_icon.hpp"
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
  std::map<int, WindowOptions> listeners;
  std::map<int, Json> inspectors;
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
  }
  ~LinuxProcess() {
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
        if (op == "desktop-result") {
          auto it = desktop_.find(message.at("request").get<std::string>());
          if (it != desktop_.end()) { auto reply = std::move(it->second.reply); desktop_.erase(it); reply(message.at("response")); }
          return;
        }
        if (op == "ready") {
          if (message.value("protocol", 0) != 1 || message.value("version", "") != REAWEB_VERSION)
            throw std::runtime_error("WebKit helper version does not match the extension. Install both files from the same release");
          version = message.value("browserVersion", ""); ready_ = true; return;
        }
        if (op == "parked") { parked_.insert(message.at("id").get<int>()); return; }
        auto it = listeners.find(message.at("id").get<int>());
        if (it == listeners.end()) return;
        if (op == "devtools-state") { inspectors[it->first] = message.at("state"); return; }
        if (op == "message") it->second.on_message(message.at("message").get<std::string>());
        else if (op == "dock-toggle" && it->second.on_dock_toggle) it->second.on_dock_toggle();
        else if (op == "drop" && it->second.on_drop) it->second.on_drop(message.at("payload"));
        else if (op == "error") it->second.on_error(message.at("error").get<std::string>());
        else if (op == "navigating" && it->second.on_navigation) it->second.on_navigation();
        else if (op == "reload-request") {
          if (!it->second.on_reload || !it->second.on_reload()) send({{"id", message.at("id")}, {"op", "reload"}});
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
};
class LinuxWindow final : public Window {
  std::shared_ptr<LinuxProcess> process_;
  int id_;
  std::unique_ptr<SwellWindow> window_;
  Json geometry_;
  mutable Json normal_;
  bool maximized_ = false;
  LinuxIcon icon_;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
public:
  unsigned native_state() const {
    using State = unsigned (*)(void*);
    static auto state = reinterpret_cast<State>(dlsym(RTLD_DEFAULT, "gdk_window_get_state"));
    auto native = SWELL_GetOSWindow(static_cast<HWND>(window_->handle()), "GdkWindow");
    return state && native ? state(native) : 0;
  }
  LinuxWindow(std::shared_ptr<LinuxProcess> process, int id, WindowOptions options) : process_(std::move(process)), id_(id) {
    window_ = std::make_unique<SwellWindow>(options.title, options.parent, [this] {
      try { process_->send({{"id", id_}, {"op", "focus"}}); } catch (...) {}
    }, options.on_close);
    process_->send({{"id", id_}, {"op", "open"}, {"uri", options.url.empty() ? file_uri(options.entry) : options.url},
      {"script", options.script}, {"lifecycleReload", static_cast<bool>(options.on_reload)}, {"dockEnabled", static_cast<bool>(options.on_dock_toggle)}});
    process_->listeners.emplace(id_, std::move(options));
  }
  ~LinuxWindow() override {
    process_->listeners.erase(id_);
    process_->inspectors.erase(id_);
    try {
      // Detach before SWELL destroys the X11 parent, preserving other pages.
      process_->park(id_);
      process_->send({{"id", id_}, {"op", "close"}});
    } catch (...) {}
  }
  void sync() {
    if (closed()) return;
    icon_.refresh(icon_target(), docked());
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
    ScreenToClient(ancestor, &origin);
    const auto& options = process_->listeners.at(id_);
    Json next = {{"id", id_}, {"op", "geometry"}, {"parent", get_xid(native)}, {"x", origin.x}, {"y", origin.y},
      {"width", rect.right - rect.left}, {"height", std::max(1, int(rect.bottom - rect.top))}, {"visible", visible()}, {"focused", focused()},
      {"docked", options.is_docked && options.is_docked()}};
    if (geometry_ != next) { process_->send(next); geometry_ = std::move(next); }
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
  void prepare_dock() override {
    normal_ = placement(); maximized_ = (native_state() & 4) != 0;
    if (maximized_) ShowWindow(static_cast<HWND>(window_->handle()), SW_RESTORE);
    window_->prepare_dock(); prepare_undock();
  }
  void prepare_undock() override {
    icon_.refresh(nullptr);
    if (!geometry_.is_null()) {
      // Invalidate even on timeout so the next pump can reattach a late acknowledgement.
      geometry_ = Json();
      process_->park(id_);
    }
  }
  void restore_floating() override {
    icon_.refresh(nullptr);
    window_->restore_floating();
    if (!normal_.is_null()) restore_placement(normal_);
    if (maximized_) ShowWindow(static_cast<HWND>(window_->handle()), SW_SHOWMAXIMIZED);
  }
  void focus() override { window_->focus(); }
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
  void* icon_target() const override {
    auto handle = static_cast<HWND>(window_->handle());
    if (!docked()) return SWELL_GetOSWindow(handle, "GdkWindow");
    if (!window_->visible()) return nullptr;
    const auto main = process_->listeners.at(id_).parent;
    while (handle && handle != main) {
      if (auto native = SWELL_GetOSWindow(handle, "GdkWindow")) return native;
      handle = GetParent(handle);
    }
    return nullptr;
  }
  void clear_icon() override { icon_.clear(icon_target(), docked()); }
  void set_icon_visible(bool visible) override { icon_.set_visible(icon_target(), visible, docked()); }
  void set_visible(bool visible) override { window_->set_visible(visible); }
  Json bounds() const override { return window_->placement(); }
  void reload() override { process_->send({{"id", id_}, {"op", "reload"}}); }
  void set_drop_enabled(bool enabled) override { process_->send({{"id", id_}, {"op", "drop-enabled"}, {"enabled", enabled}}); }
  void start_drag(const Json& payload, Reply reply) override { process_->desktop("ReaWeb_Drag", payload, std::move(reply), id_); }
  bool visible() const override { return window_->visible() && !(native_state() & 2); }
  bool focused() const override { return window_->focused(); }
  Json placement() const override {
    const bool maximized = (native_state() & 4) != 0;
    if (!maximized) normal_ = window_->placement();
    auto value = normal_.is_null() ? window_->placement() : normal_;
    if (!value.is_null()) value["maximized"] = maximized;
    return value;
  }
  void restore_placement(const Json& value) override {
    normal_ = value;
    window_->restore_placement(value);
    maximized_ = value.value("maximized", false);
    if (maximized_) ShowWindow(static_cast<HWND>(window_->handle()), SW_SHOWMAXIMIZED);
  }
  Json diagnostics() const override {
    auto inspector = devtools_state();
    auto it = process_->inspectors.find(id_);
    if (it != process_->inspectors.end()) inspector.update(it->second);
    inspector["embeddedSupported"] = true;
    return {{"backend", "WebKitGTK"}, {"browserVersion", process_->version}, {"helperProtocol", 1}, {"devtools", inspector}, {"iconError", icon_.last_error}};
  }
};
class LinuxPlatform final : public Platform {
  fs::path data_;
  std::shared_ptr<LinuxProcess> process_;
  std::vector<std::weak_ptr<LinuxWindow>> windows_;
  int next_id_ = 0;
public:
  explicit LinuxPlatform(fs::path data) : data_(std::move(data)) {}
  std::shared_ptr<Window> open(WindowOptions options) override {
    if (!process_ || (!process_->error.empty() && process_->listeners.empty())) process_ = std::make_shared<LinuxProcess>(data_);
    auto window = std::make_shared<LinuxWindow>(process_, ++next_id_, std::move(options));
    windows_.push_back(window);
    return window;
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
