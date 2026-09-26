#include "runtime/runtime.hpp"
#include "bridge_script.hpp"
#include <limits>

namespace reaweb {
int Runtime::open(const std::string& path, const fs::path& base) {
  return open_impl(path, base, "");
}

int Runtime::open_instance(const std::string& path, const std::string& instance_key, const std::string& name, bool multiple) {
  check_thread();
  if (instance_key.find('\0') != std::string::npos || name.find('\0') != std::string::npos)
    throw Error("INVALID_ARGUMENT", "Instance key and window name must not contain NUL");
  if (multiple || instance_key.empty()) return open(path);
  const auto key = std::make_pair(instance_key, name);
  // The identity lives with the session, so destruction cannot leave a stale registry entry.
  for (const auto& item : sessions_) {
    auto& session = *item.second;
    if (session.instance == key && !session.failed && is_open(session.id)) {
      focus(session.id);
      return session.id;
    }
  }
  const auto id = open(path);
  sessions_.at(id)->instance = key;
  return id;
}

int Runtime::open_dev(const std::string& url, const fs::path& base) {
  return open_impl("", base, validate_dev_url(url));
}

int Runtime::open_impl(const std::string& path, const fs::path& base, const std::string& dev_url) {
  check_thread();
  const auto directory = base.empty() ? resource_ / "Scripts" : base;
  auto entry = dev_url.empty() ? resolve_html(directory, path) : fs::absolute(directory / (".reaweb-dev-" + state_key(dev_url, 0) + ".html"));
  if (sessions_.size() >= 32) throw Error("WINDOW_LIMIT", "At most 32 ReaWebAPI windows may be open");
  if (next_id_ == std::numeric_limits<int>::max()) throw Error("WINDOW_LIMIT", "Window id space exhausted");
  const auto app_id = dev_url.empty() ? "local-" + app_identity(entry.parent_path()) : "dev-" + state_key(dev_url, 0);
  const auto profile = resource_ / "ReaWebAPI" / "Apps" / app_id;
  auto app = apps_[app_id].lock();
  if (!app) {
    app = std::make_shared<App>();
    app->id = app_id;
    app->mode = dev_url.empty() ? "app-http" : "dev-http";
    app->info = app_info(fs::canonical(entry.parent_path()), profile / "Data", app_id);
    if (dev_url.empty()) {
      app->resources = std::make_unique<WebResources>(entry.parent_path(), profile);
      app->origin = app->resources->origin();
    } else app->origin = dev_url.substr(0, dev_url.find('/', 7));
    app->platform = platform_.lock();
    if (!app->platform) {
      const auto data = resource_ / "ReaWebAPI" / "WebViewData";
      fs::create_directories(data);
      app->platform = make_platform(data);
      platform_ = app->platform;
    }
    apps_[app_id] = app;
  }
  const auto id = ++next_id_;
  auto session = std::make_shared<Session>();
  session->id = id;
  session->entry = entry;
  session->app = app;
  std::set<int> slots;
  for (const auto& item : sessions_) if (item.second->entry == entry) slots.insert(item.second->slot);
  while (slots.count(session->slot)) ++session->slot;
  auto key = state_key(entry.generic_u8string(), session->slot);
  session->ident = "ReaWebAPI:" + key;
  session->state_path = profile / "WindowState" / (key + ".json");
  session->title = "ReaWebAPI — " + entry.parent_path().filename().u8string();
  session->default_title = session->title;
  session->bridge = std::make_unique<Bridge>(host_, Bridge::Controls{
    [this, entry](const std::string& next) { return open(next, entry.parent_path()); },
    [this, id] { close(id); }, [this, id] { devtools(id); },
    [this, id](bool docked) { return set_docked(id, docked); }, [this, id] { return is_docked(id); },
    [this, id](const std::string& method, const Json& args) { return host_call(id, method, args); }
  }, std::to_string(id));
  std::weak_ptr<Session> weak = session;
  session->window = app->platform->open(WindowOptions{entry, bridge_script, session->title,
    [this, weak](std::string message) {
      if (auto s = weak.lock(); s && !s->closing) {
        Work work;
        work.session = s->id; work.generation = s->generation; work.project = project_epoch_;
        work.text = std::move(message); work.reply = true;
        if (work.text.size() > message_limit || s->outstanding >= 256) {
          fail(*s, "Bridge queue limit exceeded. Reduce the number or size of pending calls.");
          return;
        }
        if (s->ready && work.text.size() <= 4096) {
          Json input;
          try { input = parse_request(work.text); } catch (const std::exception&) {}
          if (input.is_object() && input.value("method", std::string()) == "ReaWeb_ServiceSend") {
            const auto& args = input.at("args");
            if (args.size() == 3 && args[0].is_string() && args[1].is_string() &&
                services_.is_input(args[0].get<std::string>(), args[1].get<std::string>())) {
              work.kind = Work::Request; work.data = std::move(input); work.text.clear();
              ++s->outstanding;
              Json response{{"id", work.data.at("id")}, {"document", work.data.value("document", Json())}};
              try {
                if (!work.data.contains("document") || work.data.at("document") != s->document) throw Error("DOCUMENT_STALE", "The bridge document is no longer active");
                if (work.data.contains("expiresAt")) {
                  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                  if (!work.data["expiresAt"].is_number_integer()) throw Error("INVALID_REQUEST", "Invalid request deadline");
                  if (work.data["expiresAt"].get<double>() <= static_cast<double>(now)) throw Error("REQUEST_EXPIRED", "Input request expired");
                }
                service_call(*s, work);
              } catch (const Error& e) { response["error"] = {{"code", e.code}, {"message", e.what()}}; reply(*s, work, std::move(response)); }
              catch (const std::exception& e) { response["error"] = {{"code", "SERVICE_ERROR"}, {"message", e.what()}}; reply(*s, work, std::move(response)); }
              return;
            }
          }
        }
        // Parse a small isolated request without a worker round-trip. Execution
        // still waits for tick(), outside the WebView callback. A pending call
        // keeps later requests on the worker so a large parse is never overtaken.
        if (!s->outstanding && work.text.size() <= 4096) {
          try {
            work.data = parse_request(work.text);
            work.kind = Work::Request;
            work.text.clear();
            s->queue.push_back(std::move(work));
            ++s->outstanding;
            return;
          } catch (const Error&) {
          } catch (const Json::exception&) {
          }
          // Preserve the worker's existing malformed-request error handling.
        }
        if (!worker_.submit(std::move(work)))
          fail(*s, "Bridge queue limit exceeded. Reduce the number or size of pending calls.");
        else ++s->outstanding;
      }
    }, [this, weak](std::string error) {
      if (auto s = weak.lock()) fail(*s, error);
    }, dock_.parent, [this, weak] { if (auto s = weak.lock()) navigate(*s); }, app->resources ? app->resources->entry_url(entry) : dev_url,
    [this, id] { close(id); }, [this, id] { return defer_reload(id); },
    [this, weak](Json payload) {
      auto s = weak.lock();
      if (!s || s->closing || !s->ready || !s->subscriptions.count("native-drop")) return;
      if (payload.contains("document") && payload["document"] != s->document) return;
      payload.erase("document");
      // Drops are discrete user actions, not coalesced state invalidations.
      Work output; output.kind = Work::Encode; output.session = s->id; output.generation = s->generation;
      output.counted_output = true;
      output.data = {{"document", s->document}, {"event", "native-drop"}, {"sequence", ++s->event_sequence}, {"data", payload}};
      if (s->output_pending >= 256 || !worker_.submit(std::move(output))) { fail(*s, "Native drop queue limit exceeded"); return; }
      ++s->output_pending;
    }, dock_.add && dock_.remove && dock_.index && dock_.activate ? std::function<void()>([this, weak] {
      if (auto s = weak.lock(); s && !s->closing)
        s->native_dock_request = !s->native_dock_request.value_or(is_docked(s->id));
    }) : std::function<void()>{}, [this, id] { return is_docked(id); }});
  sessions_.emplace(id, session);
  try {
    auto cached = state_cache_.find(session->ident);
    session->saved_state = cached != state_cache_.end() ? cached->second : read_state(session->state_path, entry.generic_u8string());
    if (!session->saved_state.is_null()) {
      session->window->restore_placement(session->saved_state["placement"]);
      session->window->restore_devtools(session->saved_state.value("devtools", Json()));
      const auto dock_id = session->saved_state.value("dockId", -1);
      if (dock_.remember && dock_id >= 0) dock_.remember(session->ident, dock_id);
      if (session->saved_state.value("docked", false)) set_docked(id, true);
    }
  } catch (const std::exception& e) {
    session->last_error = std::string("Window state was not restored: ") + e.what();
    log_(session->last_error);
  }
  return id;
}

bool Runtime::is_open(int id) const {
  check_thread();
  auto it = sessions_.find(id);
  return it != sessions_.end() && !it->second->closing && !it->second->window->closed();
}

bool Runtime::is_ready(int id) const {
  check_thread();
  return is_open(id) && sessions_.at(id)->ready;
}

bool Runtime::close(int id) {
  check_thread();
  if (undo_owner_ == id) finish_undo();
  auto it = sessions_.find(id);
  if (it == sessions_.end()) return false;
  if (!it->second->closing && lifecycle(*it->second, "close")) return true;
  if (!it->second->closing) it->second->closing_since = Clock::now();
  it->second->closing = true;
  clear_messages(*it->second);
  return true;
}

bool Runtime::focus(int id) {
  check_thread();
  if (!is_open(id)) return false;
  auto& window = *sessions_.at(id)->window;
  if (is_docked(id) && dock_.activate) dock_.activate(window.native_handle());
  window.focus();
  return true;
}

bool Runtime::captures_keyboard(void* handle, const std::function<bool(void*, void*)>& is_child) const {
  check_thread();
  for (const auto& item : sessions_) {
    const auto& s = *item.second;
    auto root = s.window->native_handle();
    if (!s.closing && !s.window->closed() && s.capture_keyboard && root &&
        (root == handle || is_child(root, handle))) return true;
  }
  return false;
}

bool Runtime::is_docked(int id) const {
  check_thread();
  auto it = sessions_.find(id);
  if (it == sessions_.end() || !dock_.index) return false;
  auto handle = it->second->window->native_handle();
  return handle && dock_.index(handle) >= 0;
}

bool Runtime::set_docked(int id, bool docked) {
  check_thread();
  if (!is_open(id)) throw Error("WINDOW_CLOSED", "Window is no longer open");
  auto& session = *sessions_.at(id);
  auto handle = session.window->native_handle();
  if (!handle || !dock_.index || !dock_.add || !dock_.remove || !dock_.activate)
    throw Error("DOCK_UNAVAILABLE", "REAPER docking APIs are unavailable");
  if (docked == is_docked(id)) return docked;
  if (docked) {
    persist(session);
    session.window->prepare_dock();
    dock_.add(handle, session.title, session.ident);
    if (!is_docked(id)) {
      session.window->restore_floating();
      session.icon_dirty = true; refresh_icon(session);
      throw Error("DOCK_FAILED", "REAPER did not accept the window into its Docker");
    }
    dock_.activate(handle);
  } else {
    const auto index = dock_.index(handle);
    if (dock_.remember && index >= 0) dock_.remember(session.ident, index);
    session.window->prepare_undock();
    dock_.remove(handle);
    session.window->restore_floating();
  }
  session.icon_dirty = true; refresh_icon(session);
  return is_docked(id);
}

void Runtime::detach(const Session& session) {
  if (dock_.index && dock_.remove) {
    auto handle = session.window->native_handle();
    if (handle && dock_.index(handle) >= 0) dock_.remove(handle);
  }
}

void Runtime::set_title(Session& s, const std::string& title) {
  s.window->set_title(title); s.title = title;
  if (is_docked(s.id) && dock_.refresh) dock_.refresh(s.window->native_handle());
}

void Runtime::refresh_icon(Session& s) {
  auto target = s.window->icon_target();
  if (s.icon_dirty || target != s.icon_target) {
    s.icon_target = target; s.icon_dirty = false;
    try {
      s.window->set_icon_visible(s.icon_visible);
      if (!s.icon_bitmaps.empty()) s.window->set_icon(s.icon_bitmaps);
      else if (s.icon_initialized) s.window->clear_icon();
    } catch (const std::exception& error) {
      s.last_error = error.what(); log_(s.last_error);
    }
  }
  if (!s.icon_source || s.icon_pending) return;
  auto sizes = s.window->icon_sizes();
  if (sizes == s.icon_sizes) return;
  Work work; work.kind = Work::Icon; work.session = s.id; work.generation = s.generation;
  work.icon_source = s.icon_source; work.icon_sizes = std::move(sizes); work.icon_sequence = s.icon_sequence + 1;
  if (worker_.submit(std::move(work))) { ++s.icon_sequence; s.icon_pending = true; }
}

Json Runtime::window_state(const Session& s) const {
  return {{"id", s.id}, {"title", s.title}, {"docked", is_docked(s.id)},
    {"visible", s.window->visible()}, {"focused", s.window->focused()}, {"keyboardCapture", s.capture_keyboard},
    {"iconVisible", s.icon_visible}};
}

void Runtime::persist(Session& s, bool force) {
  Json state = s.pending_state;
  if (s.window->native_handle()) {
    const bool docked = is_docked(s.id);
    Json placement;
    if (docked) {
      if (s.pending_state.is_object()) placement = s.pending_state["placement"];
      else if (s.saved_state.is_object()) placement = s.saved_state["placement"];
    }
    if (placement.is_null()) placement = s.window->placement();
    if (placement.is_null()) return;
    int index = dock_.index ? dock_.index(s.window->native_handle()) : -1;
    if (index < 0 && s.pending_state.is_object()) index = s.pending_state.value("dockId", -1);
    if (index < 0 && s.saved_state.is_object()) index = s.saved_state.value("dockId", -1);
    state = {{"schema", 1}, {"entry", s.entry.generic_u8string()}, {"placement", placement}, {"docked", docked}, {"dockId", index}};
    const auto devtools = s.window->devtools_state();
    if (!devtools.is_null()) state["devtools"] = devtools;
  } else if (!force || state.is_null()) return;
  if (state != s.pending_state) { s.pending_state = state; s.state_changed = Clock::now(); }
  if (state == s.saved_state || (!force && Clock::now() - s.state_changed < std::chrono::milliseconds(500))) return;
  Work work; work.kind = Work::Save; work.session = s.id; work.path = s.state_path; work.data = state;
  if (worker_.submit(std::move(work))) {
    s.saved_state = std::move(state);
    if (state_cache_.size() >= 256) state_cache_.erase(state_cache_.begin());
    state_cache_[s.ident] = s.saved_state;
  } else { s.last_error = "Window state save queue is full"; log_(s.last_error); }
}
}
