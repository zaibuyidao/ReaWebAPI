#include "runtime/runtime.hpp"

namespace reaweb {
void Runtime::enqueue_message(Session& s, const std::string& message, bool to_web) {
  if (message.size() > host_message_limit)
    throw Error("MESSAGE_LIMIT", "Host message exceeds 1 MiB of UTF-8 text");
  if (message.find('\0') != std::string::npos)
    throw Error("INVALID_ARGUMENT", "Host messages cannot contain NUL");
  if ((to_web ? s.web_messages : s.to_host.size()) >= host_queue_limit ||
      message.size() > host_queue_bytes - s.message_bytes)
    throw Error("QUEUE_LIMIT", "Host message queue exceeds 256 messages per direction or 16 MiB per window");
  // Validate UTF-8 before accepting text, so delivery never replaces bytes.
  try { (void)Json(message).dump(); }
  catch (const Json::exception&) { throw Error("INVALID_ARGUMENT", "Host message must be valid UTF-8"); }
  (to_web ? s.to_web : s.to_host).push_back(message);
  s.message_bytes += message.size();
  if (to_web) ++s.web_messages;
}

bool Runtime::send(int id, const std::string& message) {
  check_thread();
  if (!is_open(id)) throw Error("WINDOW_CLOSED", "Host message window is closed or unknown");
  auto& s = *sessions_.at(id);
  if (!s.ready) throw Error("NOT_READY", "Wait for ReaWeb_IsReady before sending a host message");
  enqueue_message(s, message, true);
  return true;
}

std::string Runtime::receive(int id) {
  check_thread();
  if (!is_open(id)) throw Error("WINDOW_CLOSED", "Host message window is closed or unknown");
  if (!ticking_ && !receive_dispatched_ && sessions_.at(id)->to_host.empty()) {
    // A request the page posts becomes a host message only after the WebView
    // IPC is pumped and the request is dispatched, and tick() used to be the
    // only place that did either. A Lua pass reads its messages before the
    // extension timer of the same pass runs, so it saw the page's request one
    // whole pass later than the page saw Lua's frames. Pumping and dispatching
    // here, when this window has nothing queued, removes that pass.
    //
    // ticking_ is the reentrancy guard tick() uses: a native service callback
    // reached from the dispatch below can call ReaWeb_Receive again, and that
    // nested call must only read the queue. receive_dispatched_ keeps this to
    // one pump and one dispatch per pass, because a Lua pass polls up to 32
    // times in a row and each dispatch may spend the whole 2 ms budget.
    ticking_ = true;
    struct Reset { bool& value; ~Reset() { value = false; } } reset{ticking_};
    receive_dispatched_ = true;
    if (auto platform = platform_.lock()) platform->pump();
    dispatch_requests(Clock::now() + std::chrono::milliseconds(2));
  }
  auto& s = *sessions_.at(id);
  if (s.to_host.empty()) return {};
  auto message = std::move(s.to_host.front());
  s.to_host.pop_front();
  s.message_bytes -= message.size();
  return message;
}

void Runtime::clear_messages(Session& s) {
  std::deque<std::string>().swap(s.to_web);
  std::deque<std::string>().swap(s.to_host);
  s.message_bytes = s.web_messages = 0;
}

void Runtime::flush_messages(Session& s, Clock::time_point deadline) {
  if (!s.ready || s.closing || !s.subscriptions.count("message")) return;
  for (int n = 0; n < 16 && !s.to_web.empty() && s.output_pending < 16 && Clock::now() < deadline; ++n) {
    Work output; output.kind = Work::Encode; output.session = s.id; output.generation = s.generation;
    output.host_message = output.counted_output = true;
    output.message_bytes = s.to_web.front().size();
    output.data = {{"document", s.document}, {"event", "message"},
      {"sequence", s.event_sequence + 1}, {"data", s.to_web.front()}};
    if (deliver_inline(s, output, output.data)) {
      if (s.closing) return;
      s.message_bytes -= output.message_bytes;
      --s.web_messages;
    } else {
      // Retain the head on worker backpressure. Later messages cannot overtake it.
      if (!worker_.submit(std::move(output))) break;
      ++s.output_pending;
    }
    ++s.event_sequence;
    s.to_web.pop_front();
  }
}
}
