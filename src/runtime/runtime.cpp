#include "runtime/runtime.hpp"
#include "runtime/fs.hpp"
#include <algorithm>
#include <vector>

namespace reaweb {
namespace {
// Bound both serialization work and escaped output size. Stop inspecting large
// arrays/objects as soon as the budget is exhausted; they stay on the worker.
bool small_message(const Json& value, size_t& budget) {
  if (value.is_binary() || value.is_discarded()) return false;
  if (budget < 32) return false;
  budget -= 32;
  if (value.is_string()) {
    const auto size = value.get_ref<const std::string&>().size();
    if (size > budget / 6) return false;
    budget -= size * 6;
  } else if (value.is_structured()) {
    for (const auto& item : value.items()) {
      if (value.is_object()) {
        if (item.key().size() > budget / 6) return false;
        budget -= item.key().size() * 6;
      }
      if (!small_message(item.value(), budget)) return false;
    }
  }
  return true;
}
}
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
Json Runtime::web_runtime(const Session& session) const {
  return {{"contract", 1}, {"mode", session.app->mode}, {"appId", session.app->id},
    {"origin", session.app->origin}, {"storageIsolation", "app-profile"}, {"localResources", session.app->mode == "app-http"}};
}
Json Runtime::host_call(int id, const std::string& method, const Json& args) {
  auto& s = *sessions_.at(id);
  if (method == "ReaWeb_HostSend") {
    if (!args[0].is_string()) throw Error("INVALID_ARGUMENT", "Expected a host message string");
    enqueue_message(s, args[0].get_ref<const std::string&>(), false);
    return true;
  }
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
  if (method == "ReaWeb_SetIconVisible") {
    if (!args[0].is_boolean()) throw Error("INVALID_ARGUMENT", "Expected an icon visibility boolean");
    const auto visible = args[0].get<bool>();
    s.window->set_icon_visible(visible);
    s.icon_visible = visible;
    return true;
  }
  if (method == "ReaWeb_SetTitle") {
    if (!args[0].is_string()) throw Error("INVALID_ARGUMENT", "Expected a window title");
    auto title = args[0].get<std::string>();
    if (title.empty() || title.size() > 256 || title.find('\0') != std::string::npos)
      throw Error("INVALID_ARGUMENT", "Title must contain 1 to 256 UTF-8 bytes without NUL");
    set_title(s, title); s.title_explicit = true;
    return true;
  }
  if (method == "ReaWeb_SetKeyboardCapture") {
    if (!args[0].is_boolean()) throw Error("INVALID_ARGUMENT", "Expected a boolean");
    s.capture_keyboard = args[0].get<bool>();
    return s.capture_keyboard;
  }
  if (method == "ReaWeb_OpenDev") return open_dev(args[0].get<std::string>(), s.entry.parent_path());
  if (method == "ReaWeb_BeginUndo" || method == "ReaWeb_EndUndo") return transaction_call(id, method, args);
  if (is_file_method(method) || method == "ReaWeb_SetIcon" || method == "ReaWeb_ClipboardReadText" ||
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
bool Runtime::start_async(Session& session, const Work& request) {
  const auto method = request.data.at("method").get<std::string>();
  const auto& args = request.data.at("args");
  if (method == "ReaWeb_DocumentTitle") {
    if (args.size() != 1 || !args[0].is_string())
      throw Error("INVALID_ARGUMENT", "Expected a document title");
    auto title = args[0].get<std::string>();
    if (title.find('\0') != std::string::npos)
      throw Error("INVALID_ARGUMENT", "Document title must not contain NUL");
    if (title.size() > 256) {
      size_t end = 256;
      while ((static_cast<unsigned char>(title[end]) & 0xc0) == 0x80) --end;
      title.resize(end);
    }
    if (!session.title_explicit) set_title(session, title.empty() ? session.default_title : title);
    reply(session, request, {{"id", request.data.at("id")}, {"document", request.data.at("document")},
      {"result", !session.title_explicit}});
    return true;
  }
  if (method == "ReaWeb_Favicon") {
    if (args.size() != 1 || !args[0].is_object() || !args[0].contains("revision") ||
        !args[0]["revision"].is_number_integer() || args[0]["revision"].get<double>() < 1 ||
        args[0]["revision"].get<double>() > 9007199254740991.0)
      throw Error("INVALID_ARGUMENT", "Expected a favicon revision");
    const auto& value = args[0];
    const auto revision = value["revision"].get<uint64_t>();
    const auto finish = [&](bool applied) {
      reply(session, request, {{"id", request.data.at("id")}, {"document", request.data.at("document")}, {"result", applied}});
    };
    if (session.icon_explicit || revision < session.favicon_revision) { finish(false); return true; }
    if (!value.contains("icon")) {
      if (revision == session.favicon_revision) { finish(false); return true; }
      session.favicon_revision = revision;
      ++session.icon_sequence; session.icon_pending = false;
      finish(true); return true;
    }
    if (revision != session.favicon_revision) { finish(false); return true; }
    if (value["icon"].is_null()) {
      ++session.icon_sequence; session.icon_pending = false;
      if (session.icon_source) session.window->clear_icon();
      session.icon_source.reset(); session.icon_sizes.clear(); session.icon_bitmaps.clear();
      session.icon_initialized = true; session.icon_explicit_source = false;
      session.icon_target = session.window->icon_target(); session.icon_dirty = false;
      finish(true); return true;
    }
    Work icon = request; icon.kind = Work::Icon; icon.text.clear(); icon.icon_from_page = true;
    icon.icon_sizes = session.window->icon_sizes(); icon.icon_sequence = session.icon_sequence + 1;
    if (!worker_.submit(std::move(icon))) throw Error("QUEUE_LIMIT", "Icon queue is full");
    ++session.icon_sequence; session.icon_pending = true;
    return true;
  }
  if (method == "ReaWeb_SetIcon") {
    if (args.size() != 1 || !args[0].is_string()) throw Error("INVALID_ARGUMENT", "Expected one PNG, ICO or SVG path");
    Work icon = request; icon.kind = Work::Icon; icon.text.clear();
    icon.path = fs::u8path(session.app->info.at("rootPath").get<std::string>());
    icon.icon_sizes = session.window->icon_sizes(); icon.icon_sequence = session.icon_sequence + 1;
    if (!worker_.submit(std::move(icon))) throw Error("QUEUE_LIMIT", "Icon queue is full");
    ++session.icon_sequence; session.icon_pending = true; session.icon_explicit = true;
    return true;
  }
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
bool Runtime::deliver_inline(Session& session, const Work& work, const Json& response) {
  // Do not overtake an older encoded reply/event, or reply to a replaced page.
  if (session.output_pending || work.generation != session.generation || session.window->closed()) return false;
  size_t budget = 16 * 1024;
  if (!small_message(response, budget)) return false;
  try {
    session.window->evaluate("window.__reawebReceive(" + response.dump(-1, ' ', true, Json::error_handler_t::replace) + ");");
    if (work.reply && session.outstanding) --session.outstanding;
  } catch (const std::exception& error) { fail(session, error.what()); }
  return true;
}
void Runtime::reply(Session& session, Work work, Json response) {
  if (response.contains("error")) {
    session.last_error = response["error"].value("message", "Native error");
    append_log(session, {{"level", "error"}, {"source", "native"}, {"message", session.last_error.substr(0, 8192)},
      {"code", response["error"].value("code", "NATIVE_ERROR")}, {"method", work.data.value("method", "")}, {"requestId", response.value("id", Json())}});
  }
  if (deliver_inline(session, work, response)) return;
  work.kind = Work::Encode;
  work.counted_output = true;
  work.data = std::move(response);
  work.text.clear();
  if (!worker_.submit(std::move(work))) fail(session, "Bridge output queue limit exceeded");
  else ++session.output_pending;
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
    } else if (work.kind == Work::IconReady) {
      if (work.icon_sequence != s.icon_sequence) {
        work.icon_error = {{"code", "ICON_SUPERSEDED"}, {"message", "A newer icon request replaced this request"}};
      } else {
        s.icon_pending = false;
        if (!s.closing && work.icon_error.is_null()) {
          try {
            s.window->set_icon_visible(s.icon_visible);
            s.window->set_icon(work.icon_bitmaps);
            s.icon_source = work.icon_source; s.icon_sizes = work.icon_sizes;
            s.icon_bitmaps = std::move(work.icon_bitmaps);
            s.icon_target = s.window->icon_target(); s.icon_dirty = false; s.icon_initialized = true;
            if (work.reply) s.icon_explicit_source = !work.icon_from_page;
          } catch (const Error& error) { work.icon_error = {{"code", error.code}, {"message", error.what()}}; }
          catch (const std::exception& error) { work.icon_error = {{"code", "ICON_APPLY_FAILED"}, {"message", error.what()}}; }
        }
      }
      if (work.reply && !s.closing) {
        Json response{{"id", work.data.at("id")}, {"document", work.data.at("document")}};
        if (work.icon_error.is_null()) response["result"] = true;
        else response["error"] = work.icon_error;
        reply(s, std::move(work), std::move(response));
      } else if (!s.closing && !work.icon_error.is_null() && work.icon_sequence == s.icon_sequence) {
        s.last_error = work.icon_error.value("message", "Icon refresh failed");
        // Do not repeatedly retry a failed native application at the same DPI.
        s.icon_sizes = work.icon_sizes;
        log_(s.last_error);
      }
    } else if (work.kind == Work::Script) {
      try { if (!work.host_message || !s.closing) s.window->evaluate(work.text); }
      catch (const std::exception& e) { fail(s, e.what()); }
      if (work.host_message && !s.closing) {
        s.message_bytes -= work.message_bytes;
        --s.web_messages;
      }
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
      if (!s.closing && s.native_dock_request) {
        const auto docked = *s.native_dock_request; s.native_dock_request.reset();
        try { set_docked(s.id, docked); s.window->focus(); }
        catch (const std::exception& error) { s.last_error = error.what(); log_(s.last_error); }
      }
      s.window->tick();
      if (!s.closing && !s.window->closed()) refresh_icon(s);
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
        clear_messages(s);
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
      flush_messages(s, deadline);
      if (s.output_pending < 16) {
        for (auto& event : s.events) {
          Work output; output.kind = Work::Encode; output.session = s.id; output.generation = s.generation;
          output.counted_output = true;
          output.data = {{"document", s.document}, {"event", event.first}, {"sequence", ++s.event_sequence}, {"data", event.second}};
          if (Clock::now() < deadline && deliver_inline(s, output, output.data)) continue;
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
