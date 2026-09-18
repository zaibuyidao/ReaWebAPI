#include "swell_window.hpp"
#include "linux_channel.hpp"
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
public:
  std::string error;
  std::map<int, WindowOptions> listeners;
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
        if (op == "ready") { ready_ = true; return; }
        if (op == "parked") { parked_.insert(message.at("id").get<int>()); return; }
        auto it = listeners.find(message.at("id").get<int>());
        if (it == listeners.end()) return;
        if (op == "message") it->second.on_message(message.at("message").get<std::string>());
        else if (op == "error") it->second.on_error(message.at("error").get<std::string>());
      });
      if (!ready_ && std::chrono::steady_clock::now() - started_ > std::chrono::seconds(15))
        throw std::runtime_error("WebKit process did not start within 15 seconds");
    } catch (const std::exception& failure) {
      error = std::string(failure.what()) + ". Check WebKitGTK 4.1 and the X11 display, then reopen the tool.";
      for (const auto& item : listeners) item.second.on_error(error);
    }
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
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
public:
  LinuxWindow(std::shared_ptr<LinuxProcess> process, int id, WindowOptions options) : process_(std::move(process)), id_(id) {
    window_ = std::make_unique<SwellWindow>(options.title, options.parent, [this] {
      try { process_->send({{"id", id_}, {"op", "focus"}}); } catch (...) {}
    });
    process_->send({{"id", id_}, {"op", "open"}, {"uri", file_uri(options.entry)}, {"script", options.script}});
    process_->listeners.emplace(id_, std::move(options));
  }
  ~LinuxWindow() override {
    process_->listeners.erase(id_);
    try { process_->send({{"id", id_}, {"op", "close"}}); } catch (...) {}
  }
  void sync() {
    if (closed()) return;
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
    Json next = {{"id", id_}, {"op", "geometry"}, {"parent", get_xid(native)}, {"x", origin.x}, {"y", origin.y},
      {"width", rect.right - rect.left}, {"height", rect.bottom - rect.top}, {"visible", IsWindowVisible(handle) != 0}};
    if (geometry_ != next) { process_->send(next); geometry_ = std::move(next); }
  }
  void evaluate(const std::string& script) override { process_->send({{"id", id_}, {"op", "eval"}, {"script", script}}); }
  void devtools() override { process_->send({{"id", id_}, {"op", "devtools"}}); }
  bool closed() const override { return window_->closed() || !process_->error.empty(); }
  void* native_handle() const override { return window_->handle(); }
  void prepare_dock() override { window_->prepare_dock(); prepare_undock(); }
  void prepare_undock() override {
    if (!geometry_.is_null()) {
      // Invalidate even on timeout so the next pump can reattach a late acknowledgement.
      geometry_ = Json();
      process_->park(id_);
    }
  }
  void restore_floating() override { window_->restore_floating(); }
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
