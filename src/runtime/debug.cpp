#include "runtime/runtime.hpp"

namespace reaweb {
void Runtime::fail(Session& session, const std::string& message) {
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
  entry["windowId"] = session.id;
  entry["appId"] = session.app->id;
  entry["documentGeneration"] = session.generation;
  entry["time"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  if (session.logs.size() >= 200) session.logs.erase(session.logs.begin());
  session.logs.push_back(std::move(entry));
}
}
