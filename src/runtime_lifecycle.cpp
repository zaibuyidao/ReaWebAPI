#include "runtime.hpp"

namespace reaweb {
void Runtime::append_log(Session& session, Json entry) {
  entry["windowId"] = session.id;
  entry["appId"] = session.app->id;
  entry["documentGeneration"] = session.generation;
  entry["time"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  if (session.logs.size() >= 200) session.logs.erase(session.logs.begin());
  session.logs.push_back(std::move(entry));
}
bool Runtime::lifecycle(Session& s, const std::string& action) {
  if (!s.ready || !s.lifecycle_enabled || s.failed || s.window->closed()) return false;
  if (!s.lifecycle_action.empty()) return true;
  s.lifecycle_action = action;
  s.lifecycle_token = std::to_string(++lifecycle_sequence_);
  s.lifecycle_deadline = Clock::now() + std::chrono::milliseconds(2000);
  s.window->evaluate("window.__reawebReceive(" + Json{{"document", s.document}, {"lifecycle", {
    {"event", action == "close" ? "before-close" : "before-reload"}, {"reason", action},
    {"token", s.lifecycle_token}, {"timeoutMs", 2000}}}}.dump() + ");");
  return true;
}
bool Runtime::defer_reload(int id) {
  auto it = sessions_.find(id);
  if (it == sessions_.end()) return false;
  auto& s = *it->second;
  if (s.allow_reload) { s.allow_reload = false; return false; }
  return lifecycle(s, "reload");
}
void Runtime::complete_lifecycle(Session& s) {
  if (s.lifecycle_action.empty()) return;
  const auto action = std::move(s.lifecycle_action);
  s.lifecycle_action.clear(); s.lifecycle_token.clear();
  if (action == "close") {
    if (undo_owner_ == s.id) finish_undo();
    s.audio.clear();
    s.closing = true; s.closing_since = Clock::now();
  } else {
    s.allow_reload = true;
    s.window->reload();
  }
}
}
