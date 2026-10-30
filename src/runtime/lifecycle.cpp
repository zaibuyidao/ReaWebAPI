#include "runtime/runtime.hpp"

namespace reaweb {
void Runtime::navigate(Session& session) {
  trace(session, "webview.load.begin");
  release_daw_space(session);
  if (session.generation) session.startup_timing->reset("navigation");
  session.startup_timing->mark(StartupTiming::Navigation);
  end_jsfx_edit(session);
  if(session.jsfx && session.jsfx->instance->native)session.jsfx->instance->native->close_window(session.id);
  if (session.jsfx) {
    session.jsfx->initialized = session.jsfx->presented = false;
    ++session.jsfx->activation;
    session.jsfx->last_used = Clock::now();
    session.jsfx->next_poll = {};
    session.jsfx->values = session.jsfx->changes = Json::array(); session.jsfx->scan = 0;
  }
  if (session.window) session.window->clear_media();
  streams_.detach_window(session.id);
  producers_.close_window(session.id);
  tasks_.cancel_window(session.id);
  services_.cancel_window(session.id);
  session.service_subscriptions.clear();
  if (session.window) session.window->set_drop_enabled(false);
  if (undo_owner_ == session.id) finish_undo();
  ++session.generation;
  ++session.icon_sequence; session.icon_pending = false;
  session.icon_explicit = session.icon_explicit_source; session.favicon_revision = 0;
  session.icon_dirty = true;
  if (!session.title_explicit && session.title != session.default_title) set_title(session, session.default_title);
  session.ready = false;
  session.document.clear();
  session.queue.clear();
  clear_messages(session);
  session.events.clear();
  // The replacement document starts with no Lua content. Leaving the previous
  // document's text here would attach it to the next frame it receives.
  session.lua_frame.clear();
  session.subscriptions.clear();
  session.outstanding = session.output_pending = session.events_pending = 0;
  session.capture_keyboard = true;
  session.daw_keyboard = false;
  session.audio.clear();
  session.lifecycle_enabled = false;
  session.allow_reload = false;
  session.lifecycle_action.clear(); session.lifecycle_token.clear();
  session.started = Clock::now();
  session.bridge->reset_handles();
  // Hiding a Linux hosted view can pump pending messages while parking its X11 child.
  if (session.jsfx && session.window) session.window->set_visible(false);
}
bool Runtime::lifecycle(Session& s, const std::string& action) {
  if (!s.ready || !s.lifecycle_enabled || s.failed || s.window->closed()) return false;
  if (!s.lifecycle_action.empty()) return true;
  trace(s, action == "close" ? "lifecycle.close.begin" : "lifecycle.reload.begin");
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
  trace(s, s.lifecycle_action == "close" ? "lifecycle.close.end" : "lifecycle.reload.end");
  const auto action = std::move(s.lifecycle_action);
  s.lifecycle_action.clear(); s.lifecycle_token.clear();
  if (action == "close") {
    if (undo_owner_ == s.id) finish_undo();
    s.audio.clear();
    s.closing = true; s.closing_since = Clock::now();
    s.window->clear_media();
    s.window->cancel_pending_show();
    clear_messages(s);
  } else {
    s.allow_reload = true;
    s.window->reload();
  }
}
}
