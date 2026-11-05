#include "runtime/runtime.hpp"
#include "bridge_script.hpp"
#include "platform/shared/idle_pool.hpp"
#include "platform/shared/daw_keyboard.hpp"
#include <limits>

namespace reaweb {
void Runtime::configure_webviews(unsigned idle_count) {
  check_thread();
  if (idle_count > max_idle_webviews) throw Error("INVALID_ARGUMENT", "Idle WebView count must be 0..32");
  idle_webviews_ = idle_count;
  if (auto platform = platform_.lock()) platform->set_idle_capacity(idle_count);
  if (idle_count) schedule_webview_warmup();
#ifndef _WIN32
  else warmed_platform_.reset();
#endif
}
std::shared_ptr<Platform> Runtime::prepare_platform(const std::string& app) {
  auto platform = platform_.lock();
  if (!platform) {
    const auto data = resource_ / "ReaWebAPI" / "WebViewData";
    fs::create_directories(data);
    diagnostic_log_.event(Diagnostics::Level::Debug, Diagnostics::Scope::Runtime, "platform.create.begin", app);
    platform = make_platform(data);
    diagnostic_log_.event(Diagnostics::Level::Debug, Diagnostics::Scope::Runtime, "platform.create.end", app);
    platform->set_idle_capacity(idle_webviews_);
    platform_ = platform;
  }
#ifdef _WIN32
  warmed_platform_ = platform;
#else
  if (idle_webviews_) warmed_platform_ = platform;
#endif
  return platform;
}

int Runtime::open(const std::string& path, const fs::path& base) {
  return open_impl(path, base, "");
}

int Runtime::open_ex(const std::string& path, const std::string& options) {
  const auto value = Json::parse(options, nullptr, false);
  if (!value.is_object()) throw Error("INVALID_ARGUMENT", "optionsJson must be a JSON object string");
  for (const auto& field : value.items()) {
    const auto& key = field.key();
    if ((key == "instanceKey" || key == "id" || key == "layer" || key == "mouseMode") ? !field.value().is_string() :
        (key == "multiple" || key == "hidden" || key == "frameless" || key == "resizable" || key == "transparent") ? !field.value().is_boolean() : key == "mouseRegions" ? !field.value().is_array() : true)
      throw Error("INVALID_ARGUMENT", "Unknown or invalid open option: " + key);
  }
  return open_instance(path, value.value("instanceKey", ""), value.value("id", ""),
    value.value("multiple", false), value.value("hidden", false), value.value("frameless", false), value.value("resizable", true), value.value("transparent", false), OverlayOptions::parse(value));
}

int Runtime::open_instance(const std::string& path, const std::string& instance_key, const std::string& name, bool multiple, bool hidden, bool frameless, bool resizable, bool transparent, OverlayOptions overlay) {
  check_thread();
  if (instance_key.find('\0') != std::string::npos || name.find('\0') != std::string::npos)
    throw Error("INVALID_ARGUMENT", "Instance key and window name must not contain NUL");
  if (multiple || instance_key.empty()) return open_impl(path, {}, "", instance_key, hidden, frameless, resizable, transparent, {}, overlay);
  const auto key = std::make_pair(instance_key, name);
  // The identity lives with the session, so destruction cannot leave a stale registry entry.
  for (const auto& item : sessions_) {
    auto& session = *item.second;
    if (session.instance == key && !session.failed && session.lifecycle_action != "close" && is_open(session.id)) {
      if (!hidden) focus(session.id);
      return session.id;
    }
  }
  const auto id = open_impl(path, {}, "", instance_key, hidden, frameless, resizable, transparent, {}, overlay);
  sessions_.at(id)->instance = key;
  return id;
}

int Runtime::open_dev(const std::string& url, const fs::path& base) {
  return open_impl("", base, validate_dev_url(url));
}

int Runtime::open_impl(const std::string& path, const fs::path& base, const std::string& dev_url, const std::string& launcher_source, bool hidden, bool frameless, bool resizable, bool transparent, std::shared_ptr<Session::Jsfx> jsfx, OverlayOptions overlay) {
  check_thread();
  const auto requested = Clock::now();
  std::string diagnostic_app;
  diagnostic_log_.event(Diagnostics::Level::Debug, Diagnostics::Scope::Runtime, "webview.open.begin");
  try {
  const auto directory = base.empty() ? resource_ / "Scripts" : base;
  auto entry = dev_url.empty() ? resolve_html(directory, path) : fs::absolute(directory / (".reaweb-dev-" + state_key(dev_url, 0) + ".html"));
  if (sessions_.size() >= 32) throw Error("WINDOW_LIMIT", "At most 32 ReaWebAPI windows may be open");
  if (next_id_ == std::numeric_limits<int>::max()) throw Error("WINDOW_LIMIT", "Window id space exhausted");
  const auto apps = resource_ / "ReaWebAPI" / "Apps";
  std::shared_ptr<App> app;
  Json info;
  const auto prepare = [&](const std::string& id) {
    diagnostic_app = id;
    app = apps_[(dev_url.empty() ? "app:" : "dev:") + id].lock();
    if (!app) info = app_info(fs::canonical(entry.parent_path()), apps / id / "Data", id);
  };
  auto binding = dev_url.empty() ? open_app_binding(entry.parent_path(), apps, entry,
    jsfx ? "jsfx-" + state_key(jsfx->instance->binding->ident(), 0) : "", prepare) :
    AppBinding{"dev-" + state_key(dev_url, 0), {}};
  const auto& app_id = binding.id;
  if (!dev_url.empty()) prepare(app_id);
  diagnostic_app = app_id;
  diagnostic_log_.event(Diagnostics::Level::Debug, Diagnostics::Scope::Runtime, "app.resolve.end", app_id);
  const auto profile = apps / app_id;
  const auto app_key = (dev_url.empty() ? "app:" : "dev:") + app_id;
  if (!app) {
    app = std::make_shared<App>();
    app->id = app_id;
    app->mode = dev_url.empty() ? "app-virtual" : "dev-http";
    app->launcher_source = launcher_source;
    app->info = std::move(info);
    if (dev_url.empty()) {
      app->binding = std::move(binding.lease);
      app->resources = std::make_shared<WebResources>(entry.parent_path(), app_id);
      app->origin = app->resources->origin();
    } else app->origin = dev_url.substr(0, dev_url.find('/', 7));
    app->platform = prepare_platform(app_id);
    apps_[app_key] = app;
  }
  const auto id = ++next_id_;
  auto session = std::make_shared<Session>();
  session->startup_timing->reset("create", jsfx ? jsfx->discovered : requested);
  if (jsfx) session->startup_timing->mark(StartupTiming::Discovered, jsfx->discovered);
  session->id = id;
  session->jsfx = std::move(jsfx);
  if (session->jsfx) jsfx_platform_ = app->platform;
  session->background_start = hidden;
  session->frameless = frameless;
  session->resizable = resizable;
  session->transparent = transparent;
  session->overlay = overlay;
  session->preserve_dock = transparent || overlay.floating_only();
  session->entry = entry;
  session->app = app;
  trace(*session, "instance.bind", Diagnostics::Level::Info);
  if (session->jsfx) {
    trace(*session, "jsfx.instance.identified", Diagnostics::Level::Info);
    if (!session->jsfx->instance->native_error.empty()) trace(*session, "jsfx.native.unavailable", Diagnostics::Level::Warn, session->jsfx->instance->native_error.c_str());
  }
  session->daw_keyboard = daw_keyboard_default(*session);
  std::set<int> slots;
  for (const auto& item : sessions_) if (item.second->entry == entry) slots.insert(item.second->slot);
  while (slots.count(session->slot)) ++session->slot;
  auto key = state_key(entry.generic_u8string(), session->slot);
  session->ident = "ReaWebAPI:" + key;
  session->state_path = profile / "WindowState" / (key + ".json");
  session->title = "ReaWebAPI — " + entry.parent_path().filename().u8string();
  session->default_title = session->title;
  session->bridge = std::make_unique<Bridge>(host_, Bridge::Controls{
    [this, entry, launcher = app->launcher_source](const std::string& next) { return open_impl(next, entry.parent_path(), "", launcher); },
    [this, id] { close(id); }, [this, id] { devtools(id); },
    [this, id](bool docked) { return set_docked(id, docked); }, [this, id] { return is_docked(id); },
    [this, id](const std::string& method, const Json& args) { return host_call(id, method, args); }
  }, std::to_string(id));
  std::weak_ptr<Session> weak = session;
  auto window_resources = app->resources ? std::make_shared<WebResources>(app->resources->root(), app_id) : nullptr;
  trace(*session, "webview.create.begin");
  session->window = app->platform->open(WindowOptions{entry, std::string(bridge_script) +
    (dock_.daw_space ? std::string(daw_keyboard_script) : ""), session->title,
    [this, weak](std::string message) {
      if (auto s = weak.lock(); s && !s->closing) {
        if (message.rfind("__reawebDiagnostic:", 0) == 0) { diagnostic_receive(*s, message); return; }
        if (message.rfind("__reawebDawKey:", 0) == 0) {
          if (s->ready && s->daw_keyboard_enabled() &&
              message.size() == 16 && (message.back() == 'd' || message.back() == 'r' || message.back() == 'u')) {
            auto& events = s->space_events;
            if (message.back() != 'r' || events.empty() || events.back() != 'r') {
              if (events.size() >= 64) { events.clear(); events.push_back('u'); }
              else events.push_back(message.back());
            }
          }
          return;
        }
        Work work;
        work.session = s->id; work.generation = s->generation; work.project = project_epoch_;
        work.text = std::move(message); work.reply = true;
        if (work.text.size() > message_limit || s->outstanding >= 256) {
          fail(*s, "Bridge queue limit exceeded. Reduce the number or size of pending calls.", "BRIDGE_QUEUE_LIMIT");
          return;
        }
        if (s->ready && work.text.size() <= 4096) {
          Json input;
          try { input = parse_request(work.text); } catch (const std::exception&) {}
          if (input.is_object() && (input.value("method", std::string()) == "ReaWeb_ServiceSend" ||
              (s->jsfx && !s->outstanding && input.value("method", std::string()) == "ReaWeb_ServiceInvoke"))) {
            const auto& args = input.at("args");
            if (args.size() == 3 && args[0].is_string() && args[1].is_string() &&
                ((input["method"] == "ReaWeb_ServiceSend" && services_.is_input(args[0].get<std::string>(), args[1].get<std::string>())) ||
                 (s->jsfx && input["method"] == "ReaWeb_ServiceInvoke" && args[0] == "jsfx" && args[1] == "present"))) {
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
          fail(*s, "Bridge queue limit exceeded. Reduce the number or size of pending calls.", "WORKER_QUEUE_LIMIT");
        else ++s->outstanding;
      }
    }, [this, weak](std::string error) {
      if (auto s = weak.lock()) {
        if (s->jsfx) {
          bool active = false;
          try { active = s->jsfx->instance->binding->editor_matches(s->jsfx->editor); } catch (...) {}
          if (s->closing || !active) { release_jsfx(*s); s->closing = true; return; }
        }
        const bool process_failed = error.rfind("WEBVIEW_PROCESS_FAILED:", 0) == 0;
        trace(*s, process_failed ? "webview.process.fail" : "webview.load.fail", Diagnostics::Level::Error,
          process_failed ? "WEBVIEW_PROCESS_FAILED" : "WEBVIEW_ERROR");
        fail(*s, error);
      }
    }, session->jsfx ? session->jsfx->editor.host->parent() : dock_.parent, [this, weak] { if (auto s = weak.lock()) navigate(*s); }, app->resources ? app->resources->entry_url(entry) : dev_url,
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
    }, !session->jsfx && dock_.add && dock_.remove && dock_.index && dock_.activate ? std::function<void()>([this, weak] {
      if (auto s = weak.lock(); s && !s->closing)
        s->native_dock_request = !s->native_dock_request.value_or(is_docked(s->id));
    }) : std::function<void()>{}, [this, id] { return is_docked(id); }, [weak] {
      if (auto s = weak.lock())
        return s->title != s->default_title || s->title_explicit ? s->title : s->app->info.at("name").get<std::string>();
      return std::string();
    }, window_resources, [this, weak] {
      if (auto s = weak.lock(); s && !s->closing && is_docked(s->id) && dock_.activate)
        dock_.activate(s->window->native_handle());
    }, hidden, frameless, resizable, transparent, bool(session->jsfx), session->startup_timing,
    [weak] { auto s = weak.lock(); return s && !s->closing && s->capture_keyboard; }, overlay});
  trace(*session, "webview.create.end");
  trace(*session, "webview.load.begin");
  session->window->set_media_resources(window_resources);
  if (session->jsfx) session->jsfx->editor.host->resize(session->window->native_handle());
  sessions_.emplace(id, session);
  if (session->jsfx) attach_jsfx(*session, session->jsfx->editor);
  try { if (!session->jsfx) {
    auto cached = state_cache_.find(session->ident);
    session->saved_state = cached != state_cache_.end() ? cached->second : read_state(session->state_path, entry.generic_u8string());
    if (!session->saved_state.is_null()) {
      session->window->restore_placement(session->saved_state["placement"]);
      session->window->restore_devtools(session->saved_state.value("devtools", Json()));
      const auto dock_id = session->saved_state.value("dockId", -1);
      if (!session->preserve_dock && dock_.remember && dock_id >= 0) dock_.remember(session->ident, dock_id);
      if (!session->preserve_dock && session->saved_state.value("docked", false)) {
        if (hidden) session->pending_dock = true;
        else set_docked(id, true);
      }
    }
  } } catch (const std::exception& e) {
    session->last_error = std::string("Window state was not restored: ") + e.what();
    log_(session->last_error);
  }
  // Complete native registration before a backend may present its first frame.
  try { session->window->show_after_create(); }
  catch (...) { sessions_.erase(id); throw; }
  return id;
  } catch (const Error& error) {
    diagnostic_log_.event(Diagnostics::Level::Error, Diagnostics::Scope::Runtime, "webview.open.fail", diagnostic_app, 0, error.code.c_str());
    throw;
  } catch (...) {
    diagnostic_log_.event(Diagnostics::Level::Error, Diagnostics::Scope::Runtime, "webview.open.fail", diagnostic_app, 0, "NATIVE_ERROR");
    throw;
  }
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
  if (it->second->window->presentation_pending() && it->second->lifecycle_action == "reload")
    it->second->lifecycle_action = "close";
  trace(*it->second, "webview.close.begin");
  it->second->window->cancel_pending_show();
  if (!it->second->closing && lifecycle(*it->second, "close")) return true;
  if (!it->second->closing) it->second->closing_since = Clock::now();
  it->second->closing = true;
  it->second->window->clear_media();
  clear_messages(*it->second);
  return true;
}

bool Runtime::focus(int id) {
  check_thread();
  if (!is_open(id)) return false;
  auto& session = *sessions_.at(id);
  if (session.lifecycle_action == "close") return false;
  attach_pending_dock(session);
  auto& window = *session.window;
  if (!window.presentation_pending() && is_docked(id) && dock_.activate) dock_.activate(window.native_handle());
  window.focus();
  return true;
}

void Runtime::attach_pending_dock(Session& session) {
  session.background_start = false;
  if (!session.pending_dock) return;
  session.pending_dock = false;
  try { set_docked(session.id, true); }
  catch (...) { session.pending_dock = true; session.background_start = true; throw; }
}

bool Runtime::show(int id) {
  check_thread();
  if (!is_open(id)) throw Error("WINDOW_CLOSED", "Window is no longer open");
  auto& session = *sessions_.at(id);
  if (session.lifecycle_action == "close") throw Error("WINDOW_CLOSED", "Window is closing");
  session.window->without_focus([&] {
    attach_pending_dock(session);
    session.window->show();
    if (is_docked(id) && dock_.activate) dock_.activate(session.window->native_handle());
  });
  return true;
}

bool Runtime::daw_keyboard_default(Session& session) {
  if (!session.jsfx) return app_daw_keyboard(session.entry.parent_path());
  const auto association = jsfx_association(resource_ / "Effects", session.jsfx->instance->binding->ident());
  if (!association) throw Error("JSFX_ASSOCIATION_INVALID", "The JSFX Web UI association is missing or invalid");
  session.jsfx->daw_keyboard = association->daw_keyboard;
  return association->daw_keyboard;
}

void Runtime::release_daw_space(Session& session) {
  session.space_events.clear();
  const bool down = session.space_down;
  session.space_down = false;
  if (down && dock_.daw_space) dock_.daw_space(false, false);
}
void Runtime::tick_daw_keyboard() {
  // Shortcuts may close windows or switch FX editors. Keep each session alive during dispatch.
  std::vector<std::shared_ptr<Session>> keyed;
  for (const auto& item : sessions_) if (!item.second->space_events.empty()) keyed.push_back(item.second);
  for (const auto& session : keyed) {
    if (session->closing || session->window->closed() || !session->daw_keyboard_enabled()) { release_daw_space(*session); continue; }
    while (!session->space_events.empty() && !session->closing && session->daw_keyboard_enabled()) {
      const char key = session->space_events.front(); session->space_events.pop_front();
      if ((key == 'u' || key == 'r') && !session->space_down) continue;
      if (key == 'd' && session->space_down) continue;
      if (key != 'u' && session->jsfx) {
        bool current = false;
        try { current = session->jsfx->instance->binding->editor_matches(session->jsfx->editor); } catch (const Error&) {}
        if (!current) { release_daw_space(*session); break; }
      }
      session->space_down = key != 'u';
      if (dock_.daw_space) dock_.daw_space(key != 'u', key == 'r');
    }
  }
}

bool Runtime::captures_keyboard(void* handle, const std::function<bool(void*, void*)>& is_child, bool daw_only) const {
  check_thread();
  for (const auto& item : sessions_) {
    const auto& s = *item.second;
    if (daw_only ? !s.daw_keyboard_enabled() : !s.capture_keyboard) continue;
    auto root = s.window->native_handle();
    if (!s.closing && !s.window->closed() && root &&
        (root == handle || is_child(root, handle))) return true;
  }
  return false;
}

bool Runtime::is_docked(int id) const {
  check_thread();
  auto it = sessions_.find(id);
  if (it == sessions_.end()) return false;
  if (it->second->pending_dock) return true;
  if (!dock_.index) return false;
  auto handle = it->second->window->native_handle();
  return handle && dock_.index(handle) >= 0;
}

bool Runtime::set_mouse_mode(int id, const Json& value) {
  check_thread();
  const auto mode = mouse_mode(value);
  if (!is_open(id)) throw Error("WINDOW_CLOSED", "Window is no longer open");
  auto& s = *sessions_.at(id);
  if (s.jsfx) throw Error("WINDOW_HOSTED", "Mouse policies require a standalone App");
  if (mode != "normal" && is_docked(id)) throw Error("DOCK_UNAVAILABLE", "Mouse policies require a floating window");
  s.window->set_mouse_mode(mode);
  s.overlay.mode = mode;
  return true;
}

bool Runtime::set_mouse_regions(int id, const Json& value) {
  check_thread();
  const auto regions = mouse_regions(value);
  if (!is_open(id)) throw Error("WINDOW_CLOSED", "Window is no longer open");
  auto& s = *sessions_.at(id);
  if (s.jsfx) throw Error("WINDOW_HOSTED", "Mouse policies require a standalone App");
  s.window->set_mouse_regions(regions);
  s.overlay.regions = regions;
  return true;
}

bool Runtime::set_docked(int id, bool docked) {
  check_thread();
  if (!is_open(id)) throw Error("WINDOW_CLOSED", "Window is no longer open");
  auto& session = *sessions_.at(id);
  if (session.jsfx) throw Error("WINDOW_HOSTED", "REAPER controls the JSFX editor placement");
  if (session.transparent || session.overlay.floating_only()) {
    if (docked) throw Error("DOCK_UNAVAILABLE", "Transparent, overlay and non-normal mouse policies require a standalone floating host");
    return false;
  }
  auto handle = session.window->native_handle();
  if (!handle || !dock_.index || !dock_.add || !dock_.remove || !dock_.activate)
    throw Error("DOCK_UNAVAILABLE", "REAPER docking APIs are unavailable");
  if (docked == is_docked(id)) return docked;
  if (session.background_start) { session.pending_dock = docked; return docked; }
  const bool reparents = session.window->dock_reparents_window();
  // Docking hides the native window while the Docker takes it over, so remember
  // its visibility and restore it afterwards. Backends whose native window stays
  // untouched keep their own handling.
  const bool was_visible = reparents ? session.window->visibility_requested() : true;
  if (docked) {
    persist(session);
    if (reparents) {
      // Re-register the remembered dock id before the Docker takes the window,
      // so it returns to the same slot instead of a freshly assigned one.
      int saved_dock = -1;
      if (session.pending_state.is_object()) saved_dock = session.pending_state.value("dockId", -1);
      if (saved_dock < 0 && session.saved_state.is_object()) saved_dock = session.saved_state.value("dockId", -1);
      if (dock_.remember && saved_dock >= 0) dock_.remember(session.ident, saved_dock);
    }
    session.window->prepare_dock();
    dock_.add(handle, session.title, session.ident);
    if (!is_docked(id)) {
      session.window->restore_floating();
      if (reparents && !was_visible) session.window->set_visible(false);
      session.icon_dirty = true; refresh_icon(session);
      throw Error("DOCK_FAILED", "REAPER did not accept the window into its Docker");
    }
    if (!session.window->presentation_pending()) dock_.activate(handle);
    if (reparents && was_visible) session.window->set_visible(true);
  } else {
    const auto index = dock_.index(handle);
    if (dock_.remember && index >= 0) dock_.remember(session.ident, index);
    session.window->prepare_undock();
    dock_.remove(handle);
    session.window->restore_floating();
    if (reparents && !was_visible) session.window->set_visible(false);
  }
  session.preserve_dock = false;
  session.icon_dirty = true; refresh_icon(session);
  return is_docked(id);
}

void Runtime::detach(const Session& session) {
  if (session.jsfx) return;
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
  if (s.jsfx) return;
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
    {"visible", s.window->visible()}, {"focused", s.window->focused()}, {"keyboardCapture", s.capture_keyboard}, {"dawKeyboard", s.daw_keyboard_enabled()},
    {"iconVisible", s.icon_visible}, {"frameless", s.frameless}, {"resizable", s.resizable},
    {"framelessActive", !is_docked(s.id) && s.window->frameless_active()},
    {"cornerRadius", s.corner_radius}, {"cornerRadiusActive", !is_docked(s.id) && s.window->corner_radius_active()},
    {"layer", s.overlay.layer}, {"layerActive", s.overlay.layer == "normal" || s.window->layer_active()},
    {"mouseMode", s.overlay.mode}, {"mouseModeActive", s.window->mouse_mode_active()},
    {"transparent", s.transparent}, {"transparentActive", !is_docked(s.id) && s.window->transparent_active()}};
}

void Runtime::persist(Session& s, bool force) {
  if (s.jsfx) return;
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
    // Floating-only policies preserve the saved Dock preference for the session.
    if (s.preserve_dock) state["docked"] = s.saved_state.is_object() && s.saved_state.value("docked", false);
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
