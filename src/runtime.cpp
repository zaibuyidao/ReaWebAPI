#include "runtime.hpp"
#include "bridge_script.hpp"
#include "host_io.hpp"
#include <algorithm>
#include <limits>
#include <vector>

namespace reaweb {
Runtime::Runtime(Host host, fs::path resource, std::function<void(const std::string&)> log, DockApi dock)
  : host_(std::move(host)), dock_(std::move(dock)), resource_(std::move(resource)), log_(std::move(log)), main_thread_(std::this_thread::get_id()) {
  project_ = host_.current_project();
  host_generation_ = host_.project_generation ? host_.project_generation() : 0;
  project_epoch_ = 1;
}
Runtime::~Runtime() {
  try { finish_undo(); } catch (...) {}
  for (const auto& item : sessions_) {
    try { persist(*item.second, true); detach(*item.second); } catch (...) {}
  }
  sessions_.clear();
  apps_.clear();
}
void Runtime::check_thread() const {
  if (std::this_thread::get_id() != main_thread_)
    throw Error("WRONG_THREAD", "ReaWebAPI must be called from REAPER's main thread");
}
void Runtime::fail(Session& session, const std::string& message) {
  session.last_error = message;
  session.failed = true;
  if (!session.closing) session.closing_since = Clock::now();
  session.closing = true;
  log_(message);
}
void Runtime::navigate(Session& session) {
  if (session.window) session.window->set_drop_enabled(false);
  if (undo_owner_ == session.id) finish_undo();
  ++session.generation;
  session.ready = false;
  session.document.clear();
  session.queue.clear();
  session.events.clear();
  session.subscriptions.clear();
  session.outstanding = session.output_pending = 0;
  session.capture_keyboard = true;
  session.audio.clear();
  session.lifecycle_enabled = false;
  session.allow_reload = false;
  session.lifecycle_action.clear(); session.lifecycle_token.clear();
  session.started = Clock::now();
  session.bridge->reset_handles();
}
int Runtime::open(const std::string& path, const fs::path& base) {
  return open_impl(path, base, "");
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
  auto app = apps_[app_id].lock();
  if (!app) {
    app = std::make_shared<App>();
    app->id = app_id;
    app->mode = dev_url.empty() ? "app-http" : "dev-http";
    const auto profile = resource_ / "ReaWebAPI" / "Apps" / app_id;
    app->info = app_info(fs::canonical(entry.parent_path()), profile / "Data", app_id);
    if (dev_url.empty()) {
      app->resources = std::make_unique<WebResources>(entry.parent_path(), profile);
      app->origin = app->resources->origin();
    } else app->origin = dev_url.substr(0, dev_url.find('/', 7));
    auto data = profile / "WebViewData";
    fs::create_directories(data);
    app->platform = make_platform(data);
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
  session->state_path = resource_ / "ReaWebAPI" / "WindowState" / (key + ".json");
  session->title = "ReaWebAPI — " + entry.parent_path().filename().u8string();
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
        if (work.text.size() > message_limit || s->outstanding >= 256 || !worker_.submit(std::move(work)))
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
    }});
  sessions_.emplace(id, session);
  try {
    auto cached = state_cache_.find(session->ident);
    session->saved_state = cached != state_cache_.end() ? cached->second : read_state(session->state_path, entry.generic_u8string());
    if (!session->saved_state.is_null()) {
      session->window->restore_placement(session->saved_state["placement"]);
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
  return true;
}
bool Runtime::devtools(int id) {
  check_thread();
  if (!is_open(id)) return false;
  sessions_.at(id)->window->devtools();
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
  return is_docked(id);
}
void Runtime::detach(const Session& session) {
  if (dock_.index && dock_.remove) {
    auto handle = session.window->native_handle();
    if (handle && dock_.index(handle) >= 0) dock_.remove(handle);
  }
}
Json Runtime::window_state(const Session& s) const {
  return {{"id", s.id}, {"title", s.title}, {"docked", is_docked(s.id)},
    {"visible", s.window->visible()}, {"focused", s.window->focused()}, {"keyboardCapture", s.capture_keyboard}};
}
Json Runtime::web_runtime(const Session& session) const {
  return {{"contract", 1}, {"mode", session.app->mode}, {"appId", session.app->id},
    {"origin", session.app->origin}, {"storageIsolation", "app-profile"}, {"localResources", session.app->mode == "app-http"}};
}
Json Runtime::diagnostics(int id) const {
  check_thread();
  auto it = sessions_.find(id);
  if (it == sessions_.end()) {
    auto closed = closed_diagnostics_.find(id);
    if (closed != closed_diagnostics_.end()) return closed->second;
    throw Error("WINDOW_CLOSED", "Unknown window id");
  }
  const auto& s = *it->second;
  Json result = s.window->diagnostics();
  result["webRuntime"] = web_runtime(s);
  result.update({{"version", REAWEB_VERSION}, {"protocol", 1}, {"window", window_state(s)},
    {"stage", s.closing ? "closing" : s.ready ? "ready" : "loading"}, {"documentGeneration", s.generation},
    {"projectEpoch", project_epoch_}, {"pendingCalls", s.outstanding}, {"queuedCalls", s.queue.size()},
    {"processedCalls", s.processed}, {"lastError", s.last_error}, {"schedulerBudgetMs", 2},
    {"lifecycleAction", s.lifecycle_action}, {"audioJobs", s.audio.size()}, {"recentLogs", s.logs}});
  return result;
}
Json Runtime::host_call(int id, const std::string& method, const Json& args) {
  auto& s = *sessions_.at(id);
  if (method == "ReaWeb_GetAppInfo") return s.app->info;
  if (method == "ReaWeb_GetPlatform") return runtime_platform();
  if (method == "ReaWeb_GetArchitecture") return runtime_architecture();
  if (method == "ReaWeb_GetBounds") {
    auto bounds = s.window->bounds();
    if (!bounds.is_object()) throw Error("HOST_UNAVAILABLE", "Window geometry unavailable");
    bounds["mode"] = is_docked(id) ? "docked" : "floating"; bounds["units"] = "native";
    return bounds;
  }
  if (method == "ReaWeb_SetBounds") {
    if (is_docked(id)) throw Error("WINDOW_DOCKED", "REAPER controls Docker geometry; undock before setting bounds");
    if (!args[0].is_object() || args[0].empty()) throw Error("INVALID_ARGUMENT", "Expected window bounds");
    auto bounds = s.window->bounds();
    if (!bounds.is_object()) throw Error("HOST_UNAVAILABLE", "Window geometry unavailable");
    for (const auto& field : args[0].items()) {
      const bool size = field.key() == "width" || field.key() == "height";
      if ((!size && field.key() != "x" && field.key() != "y") || !field.value().is_number_integer() ||
          field.value().get<double>() < (size ? 100 : -1000000) || field.value().get<double>() > (size ? 16384 : 1000000))
        throw Error("INVALID_ARGUMENT", "Invalid window bounds: dimensions 100..16384; coordinates -1000000..1000000");
      bounds[field.key()] = field.value();
    }
    bounds["maximized"] = false;
    const bool visible = s.window->visible();
    s.window->restore_placement(bounds);
    if (!visible) s.window->set_visible(false);
    return host_call(id, "ReaWeb_GetBounds", Json::array());
  }
  if (method == "ReaWeb_SetVisible") {
    if (!args[0].is_boolean()) throw Error("INVALID_ARGUMENT", "Expected visibility boolean");
    s.window->set_visible(args[0].get<bool>());
    if (args[0].get<bool>() && is_docked(id) && dock_.activate) dock_.activate(s.window->native_handle());
    return window_state(s);
  }
  if (method == "ReaWeb_GetTheme") return theme_colors(host_);
  if (method == "ReaWeb_GetLogs") return s.logs;
  if (method == "ReaWeb_Log") {
    const auto& entry = args[0];
    if (!entry.is_object() || !entry.contains("message") || !entry["message"].is_string() ||
        !entry.contains("level") || !entry["level"].is_string()) throw Error("INVALID_ARGUMENT", "Expected a log level and message");
    const auto level = entry["level"].get<std::string>(), message = entry["message"].get<std::string>();
    if ((level != "debug" && level != "info" && level != "warn" && level != "error") || message.size() > 16384)
      throw Error("INVALID_ARGUMENT", "Invalid log level or message larger than 16 KiB");
    append_log(s, {{"level", level}, {"source", "javascript"}, {"message", message}});
    const auto line = "[App " + std::to_string(id) + "] [" + level + "] " + message;
    auto console = host_.native_function ? reinterpret_cast<void (*)(const char*)>(host_.native_function("ShowConsoleMsg")) : nullptr;
    // Developer output is not a host failure and must not overwrite the Lua
    // ReaWeb_GetLastError value used to diagnose failed extension operations.
    if (console) console(("[ReaWebAPI] " + line + "\n").c_str());
    else log_(line);
    return true;
  }
  if (method == "ReaWeb_LifecycleSubscribe") {
    if (!args[0].is_boolean()) throw Error("INVALID_ARGUMENT", "Expected a lifecycle subscription boolean");
    s.lifecycle_enabled = args[0].get<bool>(); return true;
  }
  if (method == "ReaWeb_LifecycleComplete") {
    if (!args[0].is_string() || s.lifecycle_token.empty() || args[0] != s.lifecycle_token)
      throw Error("LIFECYCLE_STALE", "Cleanup token no longer active");
    complete_lifecycle(s); return true;
  }
  if (method == "ReaWeb_Reload") {
    if (!lifecycle(s, "reload")) { s.allow_reload = true; s.window->reload(); }
    return true;
  }
  if (method == "ReaWeb_GetDiagnostics") return diagnostics(id);
  if (method == "ReaWeb_GetWindowState") return window_state(s);
  if (method == "ReaWeb_Focus") return focus(id);
  if (method == "ReaWeb_SetTitle") {
    if (!args[0].is_string()) throw Error("INVALID_ARGUMENT", "Expected a window title");
    auto title = args[0].get<std::string>();
    if (title.empty() || title.size() > 256 || title.find('\0') != std::string::npos)
      throw Error("INVALID_ARGUMENT", "Title must contain 1 to 256 UTF-8 bytes without NUL");
    s.window->set_title(title); s.title = std::move(title);
    if (is_docked(id) && dock_.refresh) dock_.refresh(s.window->native_handle());
    return true;
  }
  if (method == "ReaWeb_SetKeyboardCapture") {
    if (!args[0].is_boolean()) throw Error("INVALID_ARGUMENT", "Expected a boolean");
    s.capture_keyboard = args[0].get<bool>();
    return s.capture_keyboard;
  }
  if (method == "ReaWeb_OpenDev") return open_dev(args[0].get<std::string>(), s.entry.parent_path());
  if (method == "ReaWeb_BeginUndo") {
    if (undo_owner_) throw Error("UNDO_BUSY", "Another managed Undo gesture is active");
    if (!args[0].is_string()) throw Error("INVALID_ARGUMENT", "Expected an Undo label");
    auto label = args[0].get<std::string>();
    if (label.empty() || label.size() > 256 || label.find('\0') != std::string::npos) throw Error("INVALID_ARGUMENT", "Invalid Undo label");
    if (!host_.begin_undo || !host_.end_undo) throw Error("UNDO_UNAVAILABLE", "Undo groups are unavailable");
    auto project = host_.current_project();
    host_.begin_undo(project);
    undo_owner_ = id; undo_project_ = project; undo_label_ = std::move(label);
    undo_token_ = std::to_string(id) + ":" + std::to_string(++undo_sequence_);
    undo_deadline_ = Clock::now() + std::chrono::seconds(30);
    return undo_token_;
  }
  if (method == "ReaWeb_EndUndo") {
    if (!args[0].is_string() || undo_owner_ != id || args[0] != undo_token_) throw Error("STALE_UNDO", "The Undo gesture already ended or belongs to another page");
    finish_undo(); return true;
  }
  if (is_file_method(method) || method == "ReaWeb_ClipboardReadText" ||
      method == "ReaWeb_ClipboardWriteText" || method == "ReaWeb_OpenExternal")
    throw Error("INVALID_REQUEST", "This host operation requires asynchronous dispatch");
  if (!args[0].is_string()) throw Error("INVALID_ARGUMENT", "Expected an event name");
  auto name = args[0].get<std::string>();
  if (std::find(runtime_events().begin(), runtime_events().end(), name) == runtime_events().end())
    throw Error("UNKNOWN_EVENT", "Unknown host event");
  if (method == "ReaWeb_Unsubscribe") {
    if (name == "native-drop") s.window->set_drop_enabled(false);
    s.subscriptions.erase(name); s.events.erase(name); return true;
  }
  if (name == "native-drop") s.window->set_drop_enabled(true);
  s.subscriptions.insert(name);
  next_observation_ = Clock::now();
  if (name == "windowstatechange") return window_state(s);
  if (name == "projectchange") return project_event_;
  if (name == "itemselectionchange") return item_event_;
  if (name == "takeselectionchange") return take_event_;
  if (name == "transportchange") return transport_event_;
  if (name == "fxchange") return fx_event_;
  if (name == "selectionchange" || name == "track-selected") return selection_event_;
  if (name == "theme-changed") return theme_colors(host_);
  if (extra_events_.count(name)) return extra_events_.at(name);
  return nullptr;
}
void Runtime::finish_undo() {
  if (!undo_owner_) return;
  const auto project = undo_project_;
  const auto label = undo_label_;
  undo_owner_ = 0; undo_project_ = nullptr; undo_token_.clear(); undo_label_.clear();
  using Validate = bool (*)(void*, void*, const char*);
  auto validate = host_.native_function ? reinterpret_cast<Validate>(host_.native_function("ValidatePtr2")) : nullptr;
  if ((!validate || validate(nullptr, project, "ReaProject*")) && host_.end_undo) host_.end_undo(project, label);
  if (host_.update_arrange) host_.update_arrange();
}
bool Runtime::start_async(Session& session, const Work& request) {
  const auto method = request.data.at("method").get<std::string>();
  const auto& args = request.data.at("args");
  if (method == "ReaWeb_AudioFileInfo" || method == "ReaWeb_AudioWaveform") {
    size_t pending = 0; for (const auto& item : sessions_) pending += item.second->audio.size();
    if (pending >= 8) throw Error("QUEUE_LIMIT", "At most eight audio jobs may be pending");
    session.audio.push_back({request, std::make_unique<AudioJob>(host_, session.entry.parent_path(), args, method == "ReaWeb_AudioWaveform")});
    return true;
  }
  if (is_file_method(method)) {
    Work file = request; file.kind = Work::File; file.path = session.entry.parent_path(); file.text.clear(); file.counted_output = true;
    if (!worker_.submit(std::move(file))) throw Error("QUEUE_LIMIT", "File operation queue is full");
    ++session.output_pending;
    return true;
  }
  const bool drag = method == "ReaWeb_DragFiles" || method == "ReaWeb_DragText";
  if (!drag && method != "ReaWeb_RevealPath" && method != "ReaWeb_ClipboardReadText" && method != "ReaWeb_ClipboardWriteText" && method != "ReaWeb_OpenExternal") return false;
  std::weak_ptr<Session> weak = sessions_.at(session.id);
  auto complete = [this, weak, request](Json result) mutable {
    auto s = weak.lock();
    if (!s || s->closing || s->generation != request.generation || s->window->closed()) return;
    Json response{{"id", request.data.at("id")}, {"document", request.data.at("document")}};
    response.update(result);
    reply(*s, std::move(request), std::move(response));
  };
  if (drag) {
    if (drag_owner_) throw Error("DRAG_BUSY", "A native drag is already active");
    auto payload = drag_payload(session.entry.parent_path(), method, args);
    drag_owner_ = session.id;
    try {
      session.window->start_drag(payload, [this, owner = session.id, complete = std::move(complete)](Json value) mutable {
        if (drag_owner_ == owner) drag_owner_ = 0;
        complete(std::move(value));
      });
    } catch (...) { drag_owner_ = 0; throw; }
    return true;
  }
  if (method == "ReaWeb_RevealPath") {
    if (args.size() != 1) throw Error("INVALID_ARGUMENT", "Expected one local path");
    auto path = existing_local_path(session.entry.parent_path(), args[0]);
    session.app->platform->desktop(method, Json::array({path.u8string()}), std::move(complete));
    return true;
  }
  {
    if (method == "ReaWeb_ClipboardReadText") {
      if (!args.empty()) throw Error("INVALID_ARGUMENT", "ClipboardReadText takes no arguments");
    } else {
      if (args.size() != 1 || !args[0].is_string()) throw Error("INVALID_ARGUMENT", "Expected one text argument");
      const auto text = args[0].get<std::string>();
      if (text.size() > value_limit || text.find('\0') != std::string::npos) throw Error("INVALID_ARGUMENT", "Text contains NUL or exceeds 16 MiB");
      if (method == "ReaWeb_OpenExternal") validate_external_url(text);
    }
    session.app->platform->desktop(method, args, std::move(complete));
  }
  return true;
}
void Runtime::reply(Session& session, Work work, Json response) {
  if (response.contains("error")) {
    session.last_error = response["error"].value("message", "Native error");
    append_log(session, {{"level", "error"}, {"source", "native"}, {"message", session.last_error.substr(0, 8192)},
      {"code", response["error"].value("code", "NATIVE_ERROR")}, {"method", work.data.value("method", "")}, {"requestId", response.value("id", Json())}});
  }
  work.kind = Work::Encode;
  work.counted_output = true;
  work.data = std::move(response);
  work.text.clear();
  if (!worker_.submit(std::move(work))) fail(session, "Bridge output queue limit exceeded");
  else ++session.output_pending;
}
void Runtime::emit(Session& session, const std::string& name, Json data) {
  if (session.ready && !session.closing && (name == "projectchange" || session.subscriptions.count(name)))
    session.events[name] = std::move(data);
}
void Runtime::observe(Clock::time_point deadline) {
  const auto project = host_.current_project();
  const auto generation = host_.project_generation ? host_.project_generation() : 0;
  if (project_ != project || host_generation_ != generation) {
    const bool loaded = host_generation_ != generation;
    finish_undo();
    item_index_ = 0; item_count_ = -1; item_event_ = take_event_ = transport_event_ = fx_event_ = nullptr;
    project_ = project;
    host_generation_ = generation;
    ++project_epoch_;
    project_changes_ = -1;
    selection_index_ = 0; selection_count_ = -1; last_selection_hash_ = 0;
    selection_event_ = nullptr;
    if (loaded) for (auto& item : sessions_) item.second->bridge->reset_handles();
    if (loaded) for (auto& item : sessions_) emit(*item.second, "project-loaded", {{"projectEpoch", project_epoch_}});
    next_observation_ = Clock::now();
  }
  if (Clock::now() < next_observation_) return;
  const auto changes = host_.change_count ? host_.change_count(project_) : 0;
  observe_extra(deadline, changes);
  Json next{{"projectEpoch", project_epoch_}, {"changeCount", changes}};
  if (next != project_event_) {
    project_event_ = next;
    for (auto& item : sessions_) emit(*item.second, "projectchange", next);
  }
  if (changes != project_changes_) {
    project_changes_ = changes; selection_index_ = 0; selection_count_ = -1;
    item_index_ = 0; item_count_ = -1;
  }
  bool wanted = false;
  for (const auto& item : sessions_) wanted = wanted || item.second->subscriptions.count("selectionchange") || item.second->subscriptions.count("track-selected");
  if (wanted) {
    const auto count = host_.count_selected_tracks(project_);
    if (count != selection_count_ || selection_index_ == 0) {
      selection_count_ = count; selection_index_ = 0; selection_hash_ = 14695981039346656037ull;
    }
    // Scan large selections incrementally. Events invalidate a selection, they do not copy every track.
    for (int n = 0; n < 64 && selection_index_ < count && Clock::now() < deadline; ++n, ++selection_index_) {
      auto track = host_.get_selected_track(project_, selection_index_);
      if (!track || !host_.valid_track(project_, track)) { selection_index_ = 0; selection_count_ = -1; return; }
      for (auto byte : host_.track_guid(track)) { selection_hash_ ^= byte; selection_hash_ *= 1099511628211ull; }
    }
    if (selection_index_ < count) return;
    if (last_selection_hash_ != selection_hash_ || selection_event_.is_null()) {
      last_selection_hash_ = selection_hash_;
      selection_event_ = {{"projectEpoch", project_epoch_}, {"revision", ++selection_revision_}, {"count", count}};
      for (auto& item : sessions_) { emit(*item.second, "selectionchange", selection_event_); emit(*item.second, "track-selected", selection_event_); }
    }
    selection_index_ = 0;
  }
  auto subscribed = [&](const char* name) {
    for (const auto& item : sessions_) if (item.second->subscriptions.count(name)) return true;
    return false;
  };
  if (host_.event_snapshot) for (const auto* name : {"transportchange", "fxchange"}) {
    bool transport = std::string(name) == "transportchange";
    if ((!subscribed(name) && !(transport && (subscribed("playback-state-changed") || subscribed("tempo-changed")))) || Clock::now() >= deadline) continue;
    auto state = host_.event_snapshot(name);
    if (!state.is_object()) continue;
    state["projectEpoch"] = project_epoch_;
    if (std::string(name) == "fxchange") state["changeCount"] = changes;
    auto& previous = std::string(name) == "fxchange" ? fx_event_ : transport_event_;
    if (transport) for (const auto* event : {"playback-state-changed", "tempo-changed"}) {
      const auto key = std::string(event) == "tempo-changed" ? "tempo" : "state";
      Json snapshot{{"projectEpoch", project_epoch_}, {"available", state.value("available", false)}};
      if (state.contains(key)) snapshot[key] = state[key];
      if (!extra_events_.count(event) || extra_events_[event] != snapshot) {
        extra_events_[event] = snapshot; for (auto& item : sessions_) emit(*item.second, event, snapshot);
      }
    }
    if (state != previous) { previous = state; for (auto& item : sessions_) emit(*item.second, name, state); }
  }
  if (host_.count_selected_items && host_.item_identity && (subscribed("itemselectionchange") || subscribed("takeselectionchange"))) {
    const auto count = host_.count_selected_items(project_);
    if (item_count_ != count || item_index_ == 0) {
      item_count_ = count; item_index_ = 0; take_count_ = 0;
      item_hash_ = take_hash_ = 14695981039346656037ull;
    }
    auto hash = [](uint64_t& state, const std::string& text) { for (unsigned char c : text) { state ^= c; state *= 1099511628211ull; } state ^= 255; state *= 1099511628211ull; };
    for (int n = 0; n < 64 && item_index_ < count && Clock::now() < deadline; ++n, ++item_index_) {
      const auto identity = host_.item_identity(project_, item_index_);
      if (identity.first.empty()) { item_index_ = 0; item_count_ = -1; return; }
      hash(item_hash_, identity.first); hash(take_hash_, identity.second);
      if (!identity.second.empty()) ++take_count_;
    }
    if (item_index_ < count) return;
    if (item_event_.is_null() || item_hash_ != last_item_hash_) {
      last_item_hash_ = item_hash_;
      item_event_ = {{"projectEpoch", project_epoch_}, {"revision", ++item_revision_}, {"count", count}};
      for (auto& item : sessions_) emit(*item.second, "itemselectionchange", item_event_);
    }
    if (take_event_.is_null() || take_hash_ != last_take_hash_) {
      last_take_hash_ = take_hash_;
      take_event_ = {{"projectEpoch", project_epoch_}, {"revision", ++take_revision_}, {"count", take_count_}};
      for (auto& item : sessions_) emit(*item.second, "takeselectionchange", take_event_);
    }
    item_index_ = 0;
  }
  next_observation_ = Clock::now() + std::chrono::milliseconds(100);
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
void Runtime::tick() {
  check_thread();
  if (ticking_) return;
  ticking_ = true;
  struct Reset { bool& value; ~Reset() { value = false; } } reset{ticking_};
  const auto deadline = Clock::now() + std::chrono::milliseconds(2);
  for (auto it = apps_.begin(); it != apps_.end();) {
    if (auto app = it->second.lock()) { app->platform->pump(); ++it; }
    else it = apps_.erase(it);
  }
  observe(deadline);
  if (undo_owner_ && Clock::now() >= undo_deadline_) finish_undo();
  Work work;
  for (int n = 0; n < 128 && Clock::now() < deadline && worker_.take(work); ++n) {
    auto it = sessions_.find(work.session);
    if (it == sessions_.end()) { if (work.kind == Work::Fault) log_(work.text); continue; }
    auto& s = *it->second;
    if (work.kind == Work::Fault) {
      if (work.path.empty()) fail(s, work.text);
      else { s.last_error = work.text; log_(work.text); }
      continue;
    }
    if (work.generation != s.generation || s.window->closed()) continue;
    if (work.kind == Work::Request) {
      if (!s.closing) s.queue.push_back(std::move(work));
    } else if (work.kind == Work::Script) {
      try { s.window->evaluate(work.text); }
      catch (const std::exception& e) { fail(s, e.what()); }
      if (work.reply && s.outstanding) --s.outstanding;
      if (work.counted_output && s.output_pending) --s.output_pending;
    }
  }
  // One request per window per pass. The next tick resumes after the last serviced window.
  for (int n = 0, idle = 0; n < 64 && !sessions_.empty() && Clock::now() < deadline; ++n) {
    auto it = sessions_.upper_bound(cursor_);
    if (it == sessions_.end()) it = sessions_.begin();
    cursor_ = it->first;
    auto s = it->second;
    if (s->closing || s->window->closed() || s->queue.empty()) {
      if (++idle >= static_cast<int>(sessions_.size())) break;
      continue;
    }
    idle = 0;
    if (host_.current_project() != project_ || (host_.project_generation && host_.project_generation() != host_generation_))
      observe(deadline);
    auto request = std::move(s->queue.front()); s->queue.pop_front();
    const auto& data = request.data;
    Json response;
    try {
      const auto method = data.at("method").get<std::string>();
      const auto document = data.value("document", std::string());
      if (document.empty()) throw Error("INVALID_REQUEST", "A document token is required");
      if (Clock::now() - request.received > std::chrono::seconds(25)) throw Error("REQUEST_EXPIRED", "Request expired before native execution");
      if (data.contains("expiresAt")) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (!data["expiresAt"].is_number_integer()) throw Error("INVALID_REQUEST", "Invalid request deadline");
        if (data["expiresAt"].get<double>() <= static_cast<double>(now))
          throw Error("REQUEST_EXPIRED", "Request expired before native execution");
      }
      if (method == "__reawebHello") {
        if (data["args"] != Json::array({1})) throw Error("PROTOCOL_MISMATCH", "Unsupported bridge protocol");
        if (s->ready && s->document != document) throw Error("DOCUMENT_STALE", "Reload the page to start a new document");
        s->document = document; s->ready = true;
        auto query = data; query["method"] = "ReaWeb_GetCapabilities"; query["args"] = Json::array();
        response = s->bridge->dispatch_request(query);
        response["result"]["webRuntime"] = web_runtime(*s);
        response["result"]["windowId"] = s->id;
        response["result"]["projectEpoch"] = project_epoch_;
      } else {
        if (!s->ready || s->document != document) throw Error("DOCUMENT_STALE", "The bridge document is no longer active");
        const bool project_call = method.rfind("ReaWeb", 0) != 0 || method == "ReaWeb_Batch" ||
          method == "ReaWeb_BeginUndo" || method == "ReaWeb_GetTrackMeter";
        if (project_call && (request.project != project_epoch_ || data.value("project", project_epoch_) != project_epoch_))
          throw Error("PROJECT_CHANGED", "The current project changed. Refresh the tool state before trying again.", {{"projectEpoch", project_epoch_}});
        // Deliver directly before entering a potentially modal native call.
        // Queuing this on the worker would defer delivery until the call returns.
        s->window->evaluate("window.__reawebReceive(" + Json{{"id", data["id"]}, {"document", document}, {"started", true}}.dump() + ");");
        if (undo_owner_ && (method == "ReaWeb_Batch" ||
            (project_call && undo_owner_ != s->id) || method.rfind("Undo_", 0) == 0 || method == "PreventUIRefresh"))
          throw Error("UNDO_BUSY", "End the managed Undo gesture before batching or using another page");
        if (undo_owner_ && project_call) s->bridge->validate_managed_call(method, data.at("args"));
        if (start_async(*s, request)) { ++s->processed; continue; }
        response = s->bridge->dispatch_request(data);
        if (method == "ReaWeb_GetCapabilities" && response.contains("result")) response["result"]["webRuntime"] = web_runtime(*s);
      }
    } catch (const Error& e) { response = error_response(data, e.code, e.what(), e.details); }
    catch (const std::exception& e) { response = error_response(data, "INVALID_REQUEST", e.what()); }
    ++s->processed;
    reply(*s, std::move(request), std::move(response));
  }
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    auto& s = *it->second;
    try {
      s.window->tick();
      if (!s.lifecycle_action.empty() && Clock::now() >= s.lifecycle_deadline) {
        append_log(s, {{"level", "warn"}, {"source", "runtime"}, {"message", "Lifecycle cleanup timed out after 2000 ms"}});
        complete_lifecycle(s);
      }
      if (!s.closing && !s.window->closed() && !s.audio.empty() && Clock::now() < deadline) {
        auto& audio = s.audio.front();
        std::optional<Json> response;
        try {
          if (auto output = audio.job->step()) response = Json{{"id", audio.request.data["id"]},
            {"document", audio.request.data["document"]}, {"result", *output}};
        } catch (const Error& error) { response = error_response(audio.request.data, error.code, error.what()); }
        catch (const std::exception& error) { response = error_response(audio.request.data, "AUDIO_ERROR", error.what()); }
        if (response) { auto request = std::move(audio.request); s.audio.pop_front(); reply(s, std::move(request), *response); }
      }
      if (s.closing || s.window->closed()) {
        // Close requests get a bounded chance to flush their reply before destroying the page.
        if (s.closing && !s.window->closed() && s.output_pending && Clock::now() - s.closing_since < std::chrono::milliseconds(250)) { ++it; continue; }
        if (undo_owner_ == s.id) finish_undo();
        persist(s, true);
        auto final_state = diagnostics(s.id);
        final_state["stage"] = s.failed ? "failed" : "closed";
        final_state["window"]["visible"] = final_state["window"]["focused"] = false;
        final_state["pendingCalls"] = final_state["queuedCalls"] = 0;
        if (closed_diagnostics_.size() >= 32) closed_diagnostics_.erase(closed_diagnostics_.begin());
        closed_diagnostics_[s.id] = std::move(final_state);
        if (drag_owner_ == s.id) drag_owner_ = 0;
        detach(s); it = sessions_.erase(it); continue;
      }
      if (!s.ready && Clock::now() - s.started > std::chrono::seconds(30)) fail(s, "WebView bridge did not become ready within 30 seconds");
      auto state = window_state(s);
      if (state != s.last_state) { s.last_state = state; emit(s, "windowstatechange", std::move(state)); }
      persist(s);
      if (s.output_pending < 16) {
        for (auto& event : s.events) {
          Work output; output.kind = Work::Encode; output.session = s.id; output.generation = s.generation;
          output.counted_output = true;
          output.data = {{"document", s.document}, {"event", event.first}, {"sequence", ++s.event_sequence}, {"data", event.second}};
          if (worker_.submit(std::move(output))) ++s.output_pending;
          else { fail(s, "Bridge event queue limit exceeded"); break; }
        }
        s.events.clear();
      }
    } catch (const std::exception& e) { fail(s, e.what()); }
    ++it;
  }
}
}
