#include "runtime/runtime.hpp"
#include <regex>

namespace reaweb {
bool Runtime::service_call(Session& session, const Work& request) {
  const auto method = request.data.at("method").get<std::string>();
  if (method != "ReaWeb_ServiceInvoke" && method != "ReaWeb_ServiceSend" &&
      method != "ReaWeb_ServiceSubscribe" && method != "ReaWeb_ServiceUnsubscribe") return false;
  const auto& args = request.data.at("args");
  const bool subscription = method == "ReaWeb_ServiceSubscribe" || method == "ReaWeb_ServiceUnsubscribe";
  static const std::regex name_pattern("^[A-Za-z0-9_.-]{1,128}$");
  if (args.size() != (subscription ? 2u : 3u) || !args[0].is_string() || !args[1].is_string() ||
      !std::regex_match(args[0].get_ref<const std::string&>(), name_pattern) ||
      !std::regex_match(args[1].get_ref<const std::string&>(), name_pattern))
    throw Error("INVALID_ARGUMENT", "Expected service and method/event names (1..128 ASCII letters, digits, _, -, .)");
  const auto service = args[0].get<std::string>(), name = args[1].get<std::string>();
  std::weak_ptr<Session> weak = sessions_.at(session.id);
  Work response_work = request;
  response_work.data.erase("args"); response_work.text.clear();
  auto complete = [this, weak, request = std::move(response_work)](Json value) {
    auto s = weak.lock();
    if (!s || s->closing || s->window->closed() || s->generation != request.generation) return;
    Json response{{"id", request.data.at("id")}, {"document", request.data.at("document")}};
    response.update(value); reply(*s, request, std::move(response));
  };
  if (subscription) {
    uint64_t handle = 0;
    try { handle = services_.lookup(service); }
    catch (const Error&) { if (method == "ReaWeb_ServiceSubscribe") throw; }
    if (method == "ReaWeb_ServiceSubscribe") {
      size_t count = 0; for (const auto& item : session.service_subscriptions) count += item.second.size();
      if (count >= 256) throw Error("QUEUE_LIMIT", "Too many service subscriptions");
      session.service_subscriptions[handle].insert(name);
    } else {
      auto it = session.service_subscriptions.find(handle);
      if (it != session.service_subscriptions.end()) {
        it->second.erase(name);
        if (it->second.empty()) session.service_subscriptions.erase(it);
      }
    }
    complete({{"result", {{"handle", std::to_string(handle)}}}});
  } else if (method == "ReaWeb_ServiceInvoke") services_.call(service, name, args[2], session.id, std::move(complete));
  else { services_.call(service, name, args[2], session.id, {}); complete({{"result", true}}); }
  return true;
}
void Runtime::service_event(uint64_t handle, const std::string& service, int window, const std::string& name, Json data) {
  for (auto& item : sessions_) {
    auto& s = *item.second;
    if (!s.ready || s.closing || s.window->closed() || (window && window != s.id)) continue;
    const auto subscriptions = s.service_subscriptions.find(handle);
    if (subscriptions == s.service_subscriptions.end()) continue;
    if (name != "unloaded" && !subscriptions->second.count(name)) continue;
    Work output; output.kind = Work::Encode; output.session = s.id; output.generation = s.generation;
    output.counted_output = true;
    output.data = {{"document", s.document}, {"service", service}, {"serviceHandle", std::to_string(handle)}, {"serviceEvent", name}, {"data", data}};
    if (s.output_pending >= 256 || !worker_.submit(std::move(output))) fail(s, "Service event queue limit exceeded");
    else ++s.output_pending;
    if (name == "unloaded") s.service_subscriptions.erase(handle);
  }
}
void Runtime::observe_native(Clock::time_point deadline) {
  std::map<std::string, size_t> counts;
  for (const auto& item : sessions_) if (item.second->ready && !item.second->closing && !item.second->window->closed())
    for (const auto& name : item.second->subscriptions) ++counts[name];
  monitors_.subscriptions(counts);
  monitors_.tick(project_epoch_, deadline, [this](const auto& name, Json data) {
    for (const auto& item : sessions_) emit(*item.second, name, data);
  });
}
}
