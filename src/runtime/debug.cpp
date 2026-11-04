#include "runtime/runtime.hpp"
#include <string_view>

namespace reaweb {
void Runtime::trace(const Session& s, const char* event, Diagnostics::Level level, const char* result, Diagnostics::Scope scope, const Json& extra) noexcept {
  const auto& app = s.app->id;
  if (!diagnostic_log_.enabled(level, scope, app)) return;
  try {
    Json fields = extra.is_object() ? extra : Json::object();
    fields.update({{"generation", s.generation}, {"pendingCalls", s.outstanding}, {"queuedCalls", s.queue.size()},
      {"outputPending", s.output_pending}, {"hostMessages", s.to_host.size()}, {"webMessages", s.web_messages}});
    if (std::string_view(event) == "webview.load.end")
      fields["durationMs"] = std::chrono::duration<double, std::milli>(Clock::now() - s.started).count();
    if (s.jsfx) {
      const auto& ui = *s.jsfx;
      const auto target = ui.instance->binding->target();
      fields.update({{"fxGuid", ui.instance->binding->guid()}, {"fxIndex", ui.editor.fx},
        {"fxLocation", target.take ? "take" : target.input ? "input" : "track"},
        {"fxSessionId", ui.instance->session_id}, {"activation", ui.activation}, {"count", ui.count}, {"revision", ui.host_revision}});
    }
    diagnostic_log_.event(level, scope, event, app, s.id, result, fields);
  } catch (...) {}
}
void Runtime::configure_diagnostics(const DiagnosticSettings& settings) {
  check_thread();
  diagnostic_log_.configure(settings);
  diagnostic_tick_at = {}; diagnostic_tick_warning = {};
  for (auto& item : sessions_) update_diagnostics(*item.second);
}
void Runtime::update_diagnostics(Session& s) noexcept {
  try {
    const bool enabled = s.ready && !s.closing && diagnostic_log_.enabled(Diagnostics::Level::Debug, Diagnostics::Scope::Runtime, s.app->id);
    if (s.diagnostic_enabled == enabled) return;
    s.diagnostic_enabled = enabled;
    s.diagnostic_config_pending = true;
    s.diagnostic_pending = 0; s.diagnostic_unresponsive = false;
    s.diagnostic_page = Json::object(); s.diagnostic_slow_at = {}; s.diagnostic_next = {};
    diagnostic_tick(s, Clock::now());
  } catch (...) {}
}
void Runtime::diagnostic_tick(Session& s, Clock::time_point now) noexcept {
  try {
    if (s.diagnostic_config_pending) {
      if (now < s.diagnostic_next || !s.ready || s.closing || s.window->closed()) return;
      s.diagnostic_next = now + std::chrono::seconds(5);
      s.window->evaluate("window.__reawebReceive(" + Json{{"document", s.document}, {"diagnosticEnabled", s.diagnostic_enabled}}.dump() + ");");
      s.diagnostic_config_pending = false;
    }
    if (!s.diagnostic_enabled) return;
    if (!s.ready || s.closing || !s.lifecycle_action.empty() || s.window->closed() ||
        !s.window->visible() || (s.jsfx && !s.jsfx->editor.host)) {
      s.diagnostic_pending = 0; s.diagnostic_unresponsive = false;
      s.diagnostic_next = now + std::chrono::seconds(5);
      return;
    }
    if (s.diagnostic_pending) {
      if (!s.diagnostic_unresponsive && now - s.diagnostic_probe_at >= std::chrono::seconds(15)) {
        s.diagnostic_unresponsive = true;
        trace(s, "bridge.unresponsive", Diagnostics::Level::Warn, "PROBE_TIMEOUT", Diagnostics::Scope::Runtime,
          {{"durationMs", std::chrono::duration<double, std::milli>(now - s.diagnostic_probe_at).count()}});
      }
      return; // At most one evaluation can wait in a stalled renderer.
    }
    if (now < s.diagnostic_next) return;
    s.diagnostic_pending = ++s.diagnostic_sequence;
    s.diagnostic_probe_at = now;
    s.diagnostic_next = now + std::chrono::seconds(5);
    s.window->evaluate("window.__reawebReceive(" + Json{{"document", s.document}, {"diagnosticProbe", s.diagnostic_pending}}.dump() + ");");
  } catch (...) {
    s.diagnostic_pending = 0; s.diagnostic_next = now + std::chrono::seconds(5);
    if (s.diagnostic_enabled && now >= s.diagnostic_slow_at) {
      s.diagnostic_slow_at = now + std::chrono::seconds(30);
      trace(s, "bridge.probe.fail", Diagnostics::Level::Warn, "PROBE_SEND_FAILED");
    }
  }
}
void Runtime::diagnostic_receive(Session& s, const std::string& message) noexcept {
  try {
    if (!s.diagnostic_enabled || !s.ready || !s.diagnostic_pending || message.size() > 2048) return;
    const auto data = Json::parse(message.substr(19));
    if (!data.is_object() || data.value("document", Json()) != s.document ||
        data.value("probe", Json()) != s.diagnostic_pending) return;
    Json fields = Json::object();
    for (const auto* key : {"pagePending", "oldestAgeMs", "oldestStarted", "streamPackets", "activeStreams", "timeouts", "queueRejected", "sendFailures",
        "scriptErrors", "promiseErrors", "callbackErrors", "streamErrors", "streamCloses", "scriptLine", "scriptColumn"}) {
      const auto value = data.value(key, Json());
      if (!value.is_number_unsigned() || value.get<double>() > 9007199254740991.0) return;
      fields[key] = value;
    }
    if (data.contains("method") && data["method"].is_string()) fields["method"] = data["method"];
    if (data.contains("timeoutMethod") && data["timeoutMethod"].is_string()) fields["timeoutMethod"] = data["timeoutMethod"];
    fields["durationMs"] = std::chrono::duration<double, std::milli>(Clock::now() - s.diagnostic_probe_at).count();
    s.diagnostic_pending = 0;
    if (!s.diagnostic_unresponsive && fields["durationMs"].get<double>() >= 1000 && Clock::now() >= s.diagnostic_slow_at) {
      s.diagnostic_slow_at = Clock::now() + std::chrono::seconds(30);
      trace(s, "bridge.slow", Diagnostics::Level::Warn, "SLOW_PROBE", Diagnostics::Scope::Runtime, fields);
    }
    if (s.diagnostic_unresponsive) {
      trace(s, "bridge.responsive", Diagnostics::Level::Warn, "recovered", Diagnostics::Scope::Runtime, fields);
      s.diagnostic_unresponsive = false;
    }
    if (fields["pagePending"].get<uint64_t>() && fields["oldestAgeMs"].get<uint64_t>() >= 30000 &&
        (!s.diagnostic_page.value("pagePending", uint64_t(0)) || s.diagnostic_page.value("oldestAgeMs", uint64_t(0)) < 30000))
      trace(s, "bridge.request.stalled", Diagnostics::Level::Warn, "LONG_PENDING", Diagnostics::Scope::Runtime, fields);
    bool changed = false;
    for (const auto* key : {"timeouts", "queueRejected", "sendFailures", "scriptErrors", "promiseErrors", "callbackErrors", "streamErrors", "streamCloses"})
      if (fields[key].get<uint64_t>() > s.diagnostic_page.value(key, uint64_t(0))) changed = true;
    trace(s, changed ? "javascript.health.warning" : "bridge.health", changed ? Diagnostics::Level::Warn : Diagnostics::Level::Debug,
      changed ? "COUNTERS_CHANGED" : "ok", Diagnostics::Scope::Runtime, fields);
    s.diagnostic_page = std::move(fields);
  } catch (...) {}
}
void Runtime::open_diagnostics_folder() {
  check_thread();
  const auto& path = diagnostic_log_.directory();
  if (path.empty()) throw Error("DIAGNOSTICS_IO", "Diagnostics directory is unavailable");
  fs::create_directories(path);
  prepare_platform()->desktop("ReaWeb_RevealPath", Json::array({path.u8string()}), [this](Json result) {
    if (result.contains("error")) log_("Could not open diagnostics log folder");
  });
}

void Runtime::fail(Session& session, const std::string& message, const char* code) {
  trace(session, "runtime.fail", Diagnostics::Level::Error, code);
  if (session.jsfx) {
    release_jsfx(session);
    auto host = session.jsfx->editor.host;
    if (host && host->valid()) jsfx_blocked_[host->parent()] = host;
  }
  session.last_error = message;
  session.failed = true;
  if (!session.closing) session.closing_since = Clock::now();
  session.closing = true;
  if (session.window) session.window->cancel_pending_show();
  clear_messages(session);
  log_(message);
}

bool Runtime::devtools(int id) {
  check_thread();
  if (!is_open(id)) return false;
  sessions_.at(id)->window->devtools();
  return true;
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
  result["idleWebViews"] = s.app->platform->idle_diagnostics();
  result["startup"] = s.startup_timing->snapshot();
  result["bridgeHealth"] = s.diagnostic_page;
  result["bridgeHealth"]["enabled"] = s.diagnostic_enabled;
  result["bridgeHealth"]["probePending"] = s.diagnostic_pending != 0;
  result["bridgeHealth"]["unresponsive"] = s.diagnostic_unresponsive;
  result["startup"]["windowId"] = s.id;
  result["startup"]["documentGeneration"] = s.generation;
  if (s.jsfx) {
    if (jsfx_discovery_) result["jsfxObserver"] = jsfx_discovery_->diagnostics();
    result["startup"]["sessionId"] = s.jsfx->instance->session_id;
    result["startup"]["activation"] = s.jsfx->activation;
  }
  result["webRuntime"] = web_runtime(s);
  result["system"] = system_diagnostics();
  result["streams"] = streams_.info();
  result["audioCapture"] = producers_.diagnostics();
  result["realtime"] = realtime_.diagnostics(id);
  if (s.jsfx) result["jsfx"] = {{"cached", !s.jsfx->editor.host}, {"activation", s.jsfx->activation},
    {"presented", s.jsfx->presented}, {"fxGuid", s.jsfx->instance->binding->guid()}, {"fxIndex", s.jsfx->editor.fx},
    {"presentationMs", s.jsfx->presentation_ms}};
  result.update({{"version", REAWEB_VERSION}, {"protocol", 1}, {"window", window_state(s)},
    {"stage", s.closing ? "closing" : s.ready ? "ready" : "loading"}, {"documentGeneration", s.generation},
    {"projectEpoch", project_epoch_}, {"pendingCalls", s.outstanding}, {"queuedCalls", s.queue.size()},
    {"processedCalls", s.processed}, {"lastError", s.last_error}, {"schedulerBudgetMs", 2},
    // Frame channel. framesPushed keeps rising even when the page is idle and
    // Lua is between defers; that is the property this change adds.
    // luaFrameAgeMs reports how stale Lua's own content is.
    {"frames", {{"pushed", s.frames_pushed}, {"intervalMs", frame_interval.count()},
      {"luaWrites", s.lua_frame_writes},
      {"luaFrameAgeMs", s.lua_frame.empty() ? Json() :
        Json(std::chrono::duration<double, std::milli>(Clock::now() - s.lua_frame_at).count())}}},
    {"lifecycleAction", s.lifecycle_action}, {"audioJobs", s.audio.size()}, {"recentLogs", s.logs}});
  return result;
}

void Runtime::append_log(Session& session, Json entry) {
  try {
    const auto level = entry.value("level", "debug");
    const auto source = entry.value("source", "runtime");
    const auto severity = level == "error" ? Diagnostics::Level::Error : level == "warn" ? Diagnostics::Level::Warn :
      level == "info" ? Diagnostics::Level::Info : Diagnostics::Level::Debug;
    trace(session, source == "javascript" ? "javascript.log" : source == "native" ? "api.fail" : "lifecycle.timeout", severity,
      source == "native" ? entry.value("code", std::string("API_ERROR")).c_str() : source == "runtime" ? "TIMEOUT" : "reported",
      source == "javascript" ? Diagnostics::Scope::App : Diagnostics::Scope::Runtime,
      {{"method", entry.value("method", std::string())}, {"service", entry.value("service", std::string())},
       {"operation", entry.value("operation", std::string())}});
  } catch (...) {}

  entry["windowId"] = session.id;
  entry["appId"] = session.app->id;
  entry["documentGeneration"] = session.generation;
  entry["time"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  if (session.logs.size() >= 200) session.logs.erase(session.logs.begin());
  session.logs.push_back(std::move(entry));
}
}
