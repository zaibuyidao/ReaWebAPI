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
