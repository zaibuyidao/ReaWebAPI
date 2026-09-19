#include "runtime/runtime.hpp"

namespace reaweb {
void Runtime::fail(Session& session, const std::string& message) {
  session.last_error = message;
  session.failed = true;
  if (!session.closing) session.closing_since = Clock::now();
  session.closing = true;
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
  result["webRuntime"] = web_runtime(s);
  result.update({{"version", REAWEB_VERSION}, {"protocol", 1}, {"window", window_state(s)},
    {"stage", s.closing ? "closing" : s.ready ? "ready" : "loading"}, {"documentGeneration", s.generation},
    {"projectEpoch", project_epoch_}, {"pendingCalls", s.outstanding}, {"queuedCalls", s.queue.size()},
    {"processedCalls", s.processed}, {"lastError", s.last_error}, {"schedulerBudgetMs", 2},
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
