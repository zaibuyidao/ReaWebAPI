#include "runtime.hpp"
#include "bridge_script.hpp"
#include <vector>
#include <limits>

namespace reaweb {
Runtime::Runtime(Host host, fs::path resource, std::function<void(const std::string&)> log, DockApi dock)
  : host_(std::move(host)), dock_(std::move(dock)), resource_(std::move(resource)), log_(std::move(log)), main_thread_(std::this_thread::get_id()) {}
Runtime::~Runtime() {
  for (const auto& item : sessions_) detach(*item.second);
  sessions_.clear();
  platform_.reset();
}

void Runtime::check_thread() const {
  if (std::this_thread::get_id() != main_thread_)
    throw Error("WRONG_THREAD", "ReaWebAPI must be called from REAPER's main thread");
}
int Runtime::open(const std::string& path, const fs::path& base) {
  check_thread();
  auto entry = resolve_html(base.empty() ? resource_ / "Scripts" : base, path);
  if (sessions_.size() >= 32) throw Error("WINDOW_LIMIT", "At most 32 ReaWebAPI windows may be open");
  if (next_id_ == std::numeric_limits<int>::max()) throw Error("WINDOW_LIMIT", "Window id space exhausted");
  if (!platform_) {
    auto data = resource_ / "ReaWebAPI" / "WebViewData";
    fs::create_directories(data);
    platform_ = make_platform(data);
  }
  const auto id = ++next_id_;
  auto session = std::make_shared<Session>();
  session->entry = entry;
  session->bridge = std::make_unique<Bridge>(host_, Bridge::Controls{
    [this, entry](const std::string& path) { return open(path, entry.parent_path()); },
    [this, id] { close(id); }, [this, id] { devtools(id); },
    [this, id](bool docked) { return set_docked(id, docked); }, [this, id] { return is_docked(id); }
  }, std::to_string(id));
  std::weak_ptr<Session> weak = session;
  session->window = platform_->open(WindowOptions{entry, bridge_script,
    "ReaWebAPI — " + entry.parent_path().filename().u8string(),
    [weak](std::string message) {
      if (auto s = weak.lock(); s && !s->closing) {
        if (message.size() <= 65536 && s->queue.size() < 256) s->queue.push_back(std::move(message));
        else s->closing = true;
      }
    }, [log = log_, weak](std::string error) {
      log(error);
      if (auto s = weak.lock()) s->closing = true;
    }, dock_.parent});
  sessions_.emplace(id, std::move(session));
  return id;
}
bool Runtime::is_open(int id) const {
  check_thread();
  auto it = sessions_.find(id);
  return it != sessions_.end() && !it->second->closing && !it->second->window->closed();
}
bool Runtime::close(int id) {
  check_thread();
  auto it = sessions_.find(id);
  if (it == sessions_.end()) return false;
  it->second->closing = true;
  return true;
}
bool Runtime::devtools(int id) {
  check_thread();
  auto it = sessions_.find(id);
  if (it == sessions_.end()) return false;
  it->second->window->devtools();
  return true;
}
bool Runtime::is_docked(int id) const {
  check_thread();
  auto it = sessions_.find(id);
  if (it == sessions_.end() || it->second->window->closed() || !dock_.index) return false;
  auto handle = it->second->window->native_handle();
  return handle && dock_.index(handle) >= 0;
}
bool Runtime::set_docked(int id, bool docked) {
  check_thread();
  auto it = sessions_.find(id);
  if (it == sessions_.end() || it->second->closing || it->second->window->closed())
    throw Error("WINDOW_CLOSED", "Window is no longer open");
  auto& session = *it->second;
  auto handle = session.window->native_handle();
  if (!handle || !dock_.index || !dock_.add || !dock_.remove || !dock_.activate)
    throw Error("DOCK_UNAVAILABLE", "REAPER docking APIs are unavailable");
  if (docked == is_docked(id)) return docked;
  if (docked) {
    session.window->prepare_dock();
    dock_.add(handle, session.entry.parent_path().filename().u8string(), "ReaWebAPI:" + session.entry.generic_u8string());
    if (!is_docked(id)) {
      session.window->restore_floating();
      throw Error("DOCK_FAILED", "REAPER did not accept the window into its Docker");
    }
    dock_.activate(handle);
  } else {
    session.window->prepare_undock();
    dock_.remove(handle);
    session.window->restore_floating();
  }
  return is_docked(id);
}
void Runtime::detach(const Session& session) {
  if (dock_.index && dock_.remove) {
    auto handle = session.window->native_handle();
    if (handle && dock_.index(handle) >= 0) dock_.remove(handle);
  }
}
void Runtime::tick() {
  check_thread();
  if (ticking_) return;
  ticking_ = true;
  struct Reset { bool& value; ~Reset() { value = false; } } reset{ticking_};
  if (platform_) platform_->pump();
  // Snapshot permits a bridge call to create another window during dispatch.
  std::vector<std::shared_ptr<Session>> active;
  for (auto& item : sessions_) active.push_back(item.second);
  for (auto& s : active) {
    if (s->closing || s->window->closed()) continue;
    s->bridge->observe_project();
    for (int n = 0; n < 32 && !s->queue.empty() && !s->closing; ++n) {
      auto message = std::move(s->queue.front());
      s->queue.pop_front();
      const auto response = s->bridge->dispatch(message);
      // ASCII JSON keeps U+2028/U+2029 and arbitrary track names safe in JS source.
      s->window->evaluate("window.__reawebReceive(" + response.dump(-1, ' ', true, Json::error_handler_t::replace) + ");");
    }
  }
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (it->second->closing || it->second->window->closed()) {
      detach(*it->second);
      it = sessions_.erase(it);
    }
    else ++it;
  }
}
}
