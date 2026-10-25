#include "runtime/runtime.hpp"
#include <chrono>

namespace reaweb {
namespace {
template<class F> F function(const Host& host, const char* name) {
  return reinterpret_cast<F>(host.native_function ? host.native_function(name) : nullptr);
}
// Every read below is best effort. A REAPER build without the function, a host
// that refuses the lookup, or a function that throws costs one field of one
// frame. It must never break the timer callback that also delivers replies.
template<class F, class Read> void sample(Json& frame, const char* field, const Host& host, const char* name, Read read) {
  try {
    if (auto fn = function<F>(host, name)) frame[field] = read(fn);
  } catch (...) {}
}
}
// Runs on REAPER's main thread from Runtime::tick(), which REAPER calls on its
// extension timer. Pushing here decouples the page refresh rate from the
// ReaScript defer cadence: a slow or jittery defer no longer stalls the page, it
// only delays new content.
void Runtime::pump_frames() {
  for (auto& item : sessions_) {
    auto& s = *item.second;
    if (!s.ready || s.closing || s.window->closed()) continue;
    const auto now = Clock::now();
    if (now < s.next_frame) continue;
    // Advance from the previous deadline rather than from now, so a late timer
    // pass does not accumulate drift. A pass that overran several intervals
    // still pushes only one frame: this loop never catches up with a burst.
    s.next_frame += frame_interval;
    if (s.next_frame <= now) s.next_frame = now + frame_interval;
    Json frame = {{"sequence", ++s.frame_sequence}};
    // Read here, on the main thread, straight from REAPER. These stay fresh even
    // when Lua has not run for a while, which is what keeps the page moving
    // between defers.
    sample<int (*)()>(frame, "playing", host_, "GetPlayState", [](auto fn) { return fn(); });
    sample<double (*)()>(frame, "position", host_, "GetPlayPosition", [](auto fn) { return fn(); });
    sample<double (*)()>(frame, "tempo", host_, "Master_GetTempo", [](auto fn) { return fn(); });
    sample<double (*)()>(frame, "cursor", host_, "GetCursorPosition", [](auto fn) { return fn(); });
    // Whatever Lua last wrote, verbatim. The page merges it over the fields
    // above, so Lua keeps ownership of its own content.
    if (!s.lua_frame.empty()) {
      frame["lua"] = s.lua_frame;
      frame["luaAgeMs"] = std::chrono::duration<double, std::milli>(now - s.lua_frame_at).count();
    }
    ++s.frames_pushed;
    // Overwritten per name, never queued, so a slow page cannot build a backlog
    // out of frames and this loop never has to wait for one.
    emit(s, "frame", std::move(frame));
  }
}
// Replaces the content Lua contributes to the next frame. Deliberately not a
// message: it is overwritten, never queued, so a slow page cannot build a
// backlog and a fast page never replays stale content.
bool Runtime::set_frame(int id, const std::string& text) {
  check_thread();
  if (!is_open(id)) throw Error("WINDOW_CLOSED", "Host message window is closed or unknown");
  auto& s = *sessions_.at(id);
  if (text.size() > host_message_limit) throw Error("MESSAGE_LIMIT", "Frame content exceeds 1 MiB of UTF-8 text");
  if (text.find('\0') != std::string::npos) throw Error("INVALID_ARGUMENT", "Frame content cannot contain NUL");
  // Validate UTF-8 here so the page never receives replacement characters.
  try { (void)Json(text).dump(); }
  catch (const Json::exception&) { throw Error("INVALID_ARGUMENT", "Frame content must be valid UTF-8"); }
  s.lua_frame = text;
  s.lua_frame_at = Clock::now();
  ++s.lua_frame_writes;
  return true;
}
}
