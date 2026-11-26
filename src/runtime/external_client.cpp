#include "runtime/runtime.hpp"
#include <algorithm>
#include <regex>

namespace reaweb {
namespace {
const std::vector<std::string>& external_events() {
  static const auto names = with_runtime_event_aliases({"trackSelectionChanged", "trackStateChanged", "transportChanged",
    "projectChanged", "markersChanged", "regionsChanged", "currentRegionChanged", "loopPointsChanged", "timeSelectionChanged"});
  return names;
}
const std::vector<std::string> methods = {"auth.authenticate", "system.getInfo", "system.getCapabilities",
  "service.invoke", "service.send", "service.subscribe", "service.unsubscribe", "events.subscribe", "events.unsubscribe", "stream.open", "stream.close"};
void shape(const Json& params, std::initializer_list<const char*> required, std::initializer_list<const char*> optional = {}) {
  if (!params.is_object()) throw Error("INVALID_ARGUMENT", "params must be an object");
  for (const auto* key : required) if (!params.contains(key)) throw Error("INVALID_ARGUMENT", std::string("Missing parameter: ") + key);
  for (const auto& item : params.items()) {
    auto contains = [&](auto names) { return std::any_of(names.begin(), names.end(), [&](auto key) { return item.key() == key; }); };
    if (!contains(required) && !contains(optional)) throw Error("INVALID_ARGUMENT", "Unknown parameter: " + item.key());
  }
}
std::string name(const Json& params, const char* field) {
  static const std::regex pattern("^[A-Za-z0-9_.-]{1,128}$");
  if (!params.at(field).is_string() || !std::regex_match(params.at(field).get_ref<const std::string&>(), pattern))
    throw Error("INVALID_ARGUMENT", std::string("Invalid name: ") + field);
  return params.at(field).get<std::string>();
}
std::string identifier(const Json& params, const char* field) {
  if (!params.at(field).is_string() || params.at(field).get_ref<const std::string&>().size() > 128)
    throw Error("INVALID_ARGUMENT", "Expected an opaque identifier");
  return params.at(field).get<std::string>();
}
bool token_matches(const std::string& supplied, const std::string& expected) {
  size_t difference = supplied.size() ^ expected.size();
  for (size_t i = 0; i < expected.size(); ++i) difference |= unsigned(expected[i] ^ (i < supplied.size() ? supplied[i] : 0));
  return difference == 0 && !expected.empty();
}
}
void Runtime::configure_external(const ExternalSettings& settings) {
  check_thread();
  if (settings.enabled && (settings.token.size() != 64 || !settings.port))
    throw Error("INVALID_ARGUMENT", "External Client requires a port and a 256-bit access token");
  if (settings.enabled == external_settings_.enabled && settings.port == external_settings_.port && settings.token == external_settings_.token) return;
  std::unique_ptr<ExternalTransport> replacement;
  if (settings.enabled && (!external_transport_ || settings.port != external_settings_.port))
    replacement = std::make_unique<ExternalTransport>(settings.port);
  for (auto& item : external_sessions_) {
    item.second->connection->close(); services_.cancel_external(item.first); streams_.detach_external(item.first);
  }
  external_sessions_.clear();
  if (external_transport_) for (auto& connection : external_transport_->connections()) connection->close();
  if (!settings.enabled) external_transport_.reset();
  else if (replacement) external_transport_ = std::move(replacement);
  external_settings_ = settings;
}
uint16_t Runtime::external_port() const { return external_transport_ ? external_transport_->port() : 0; }
void Runtime::stop_external() {
  external_transport_.reset();
  for (auto& item : external_sessions_) {
    item.second->connection->close(); services_.cancel_external(item.first); streams_.detach_external(item.first);
  }
  external_sessions_.clear();
}
void Runtime::sync_external() {
  for (auto it = external_sessions_.begin(); it != external_sessions_.end();) {
    if (it->second->connection->alive && !it->second->connection->closing) { ++it; continue; }
    services_.cancel_external(it->first); streams_.detach_external(it->first);
    it = external_sessions_.erase(it);
  }
  if (!external_transport_) return;
  for (auto& connection : external_transport_->connections()) {
    if (!connection->alive || connection->closing) continue;
    bool found = false;
    for (const auto& item : external_sessions_) if (item.second->connection == connection) { found = true; break; }
    if (found) continue;
    auto session = std::make_shared<ExternalSession>();
    session->id = ++next_external_; session->identity = external_token(); session->connection = connection;
    external_sessions_.emplace(session->id, std::move(session));
  }
}
Json Runtime::external_event_data(ExternalSession& session, const std::string& event, Json data) {
  if (runtime_event_source(event) == "projectChanged" && data.contains("projects")) {
    const auto active = data.value("activeProject", "");
    data["activeProject"] = "";
    std::map<std::string, std::string> live_ids;
    for (auto& project : data["projects"]) {
      const auto id = project.at("id").get<std::string>();
      const auto previous = session.project_ids.find(id);
      const auto opaque = previous == session.project_ids.end()
        ? session.identity + ":p" + std::to_string(++session.sequence) : previous->second;
      live_ids.emplace(id, opaque);
      project["id"] = opaque;
      if (id == active) data["activeProject"] = opaque;
    }
    session.project_ids.swap(live_ids);
  }
  return data;
}
void Runtime::external_event(const std::string& event, const Json& data) {
  for (auto& item : external_sessions_) {
    auto& session = *item.second;
    for (const auto& subscription : session.subscriptions)
      if (subscription.second.service.empty() && runtime_event_source(subscription.second.event) == event)
        session.events[subscription.first] = data;
  }
}
void Runtime::flush_external_events() {
  for (auto& item : external_sessions_) {
    auto& session = *item.second;
    for (auto& event : session.events) {
      const auto subscription = session.subscriptions.find(event.first);
      if (subscription == session.subscriptions.end()) continue;
      try {
        session.connection->send({{"type", "event"}, {"subscriptionId", event.first}, {"event", subscription->second.event},
          {"data", external_event_data(session, subscription->second.event, std::move(event.second))}});
      } catch (...) { session.connection->close(); }
    }
    session.events.clear();
  }
}
void Runtime::external_service_event(uint64_t handle, const std::string& service, int window, const std::string& event, const Json& data) {
  if (window) return; // Existing positive Window IDs remain window-only targets.
  for (auto& item : external_sessions_) {
    auto& session = *item.second;
    for (auto it = session.subscriptions.begin(); it != session.subscriptions.end();) {
      const auto& subscription = it->second;
      if (subscription.handle != handle || (event != "unloaded" && subscription.event != event)) { ++it; continue; }
      session.connection->send({{"type", "event"}, {"subscriptionId", it->first}, {"service", service}, {"event", event}, {"data", data}});
      if (event == "unloaded") it = session.subscriptions.erase(it); else ++it;
    }
  }
}
bool Runtime::dispatch_external() {
  for (size_t n = 0; n < external_sessions_.size(); ++n) {
    auto it = external_sessions_.upper_bound(external_cursor_);
    if (it == external_sessions_.end()) it = external_sessions_.begin();
    external_cursor_ = it->first;
    auto session = it->second;
    ExternalConnection::Request request;
    if (!session->connection->take(request, [this](const Json& data) {
      if (data.value("type", Json()) != "request" || data.value("method", Json()) != "service.send" ||
          !data.contains("params") || !data["params"].is_object()) return false;
      const auto& params = data["params"];
      return params.contains("service") && params["service"].is_string() && params.contains("method") && params["method"].is_string() &&
        services_.is_input(params["service"].get<std::string>(), params["method"].get<std::string>());
    })) continue;
    const auto id = request.data.at("id");
    auto respond = [weak = std::weak_ptr<ExternalSession>(session), id](Json value) {
      if (auto target = weak.lock()) {
        value["type"] = "response"; value["id"] = id; target->connection->send(std::move(value));
      }
    };
    bool close = false;
    try {
      const auto& data = request.data;
      if (data.value("type", Json()) != "request" || !data.contains("method") || !data["method"].is_string() || !data.contains("params"))
        throw Error("INVALID_REQUEST", "Expected request type, id, method and params");
      const auto method = data.at("method").get<std::string>(); const auto& params = data.at("params");
      if (method == "auth.authenticate") {
        close = true;
        shape(params, {"token", "protocolVersion"});
        if (!params["protocolVersion"].is_number_integer() || params["protocolVersion"] != 1)
          throw Error("PROTOCOL_MISMATCH", "External Protocol version 1 is required");
        if (!params["token"].is_string() || !token_matches(params["token"].get_ref<const std::string&>(), external_settings_.token))
          throw Error("AUTH_FAILED", "Invalid access token");
        session->connection->authenticated = true; close = false;
        respond({{"result", {{"authenticated", true}, {"protocolVersion", 1}}}});
        return true;
      }
      if (!request.authenticated) throw Error("AUTH_REQUIRED", "Authenticate before calling this method");
      if (Clock::now() - request.received > std::chrono::seconds(25)) throw Error("REQUEST_EXPIRED", "Request expired before native execution");
      Json result;
      if (method == "system.getInfo") {
        shape(params, {});
        result = {{"version", REAWEB_VERSION}, {"protocolVersion", 1}, {"reaperVersion", host_version(host_)}, {"projectEpoch", project_epoch_}};
      } else if (method == "system.getCapabilities") {
        shape(params, {});
        result = {{"protocolVersion", 1}, {"methods", methods},
          {"services", services_.info()}, {"events", external_events()}, {"stream", streams_.info()},
          {"limits", {{"messageBytes", message_limit},
            {"connections", ExternalTransport::connection_limit}, {"pendingRequests", ExternalTransport::pending_limit},
            {"subscriptions", 256}, {"servicePayloadBytes", host_message_limit}, {"serviceTimeoutMs", 30000},
            {"streamConsumers", 64}, {"streamTicketMs", 10000}, {"authenticationTimeoutMs", 5000}}}};
      } else if (method == "service.invoke" || method == "service.send") {
        shape(params, {"service", "method"}, {"payload"});
        const auto service = name(params, "service"), operation = name(params, "method");
        services_.call_external(service, operation, params.value("payload", Json()), session->id,
          method == "service.invoke" ? ServiceRegistry::Reply(respond) : ServiceRegistry::Reply());
        if (method == "service.invoke") return true;
        result = {{"accepted", true}};
      } else if (method == "events.subscribe" || method == "service.subscribe") {
        const bool service = method == "service.subscribe";
        if (service) shape(params, {"service", "event"}); else shape(params, {"event"});
        const auto event = name(params, "event"), service_name = service ? name(params, "service") : "";
        if (jsfx_service_ && service_name == "jsfx") throw Error("FX_UNBOUND", "JSFX events require a bound Hosted WebView");
        uint64_t handle = service ? services_.lookup(service_name) : 0;
        const auto& source = service ? event : runtime_event_source(event);
        const auto& names = external_events();
        if (!service && (std::find(names.begin(), names.end(), event) == names.end() || !monitors_.contains(source)))
          throw Error("INVALID_ARGUMENT", "Unknown Native Event");
        if (session->subscriptions.size() >= 256) throw Error("QUEUE_LIMIT", "Too many subscriptions");
        const auto subscription = session->identity + ":s" + std::to_string(++session->sequence);
        session->subscriptions.emplace(subscription, ExternalSession::Subscription{event, service_name, handle});
        if (!service) { const auto snapshot = monitors_.snapshot(source); if (!snapshot.is_null() &&
          (source != "projectChanged" || snapshot.at("projectEpoch") == project_epoch_)) session->events[subscription] = snapshot; }
        result = {{"subscriptionId", subscription}};
      } else if (method == "events.unsubscribe" || method == "service.unsubscribe") {
        shape(params, {"subscriptionId"}); const auto subscription = identifier(params, "subscriptionId");
        auto found = session->subscriptions.find(subscription);
        if (found == session->subscriptions.end() || found->second.service.empty() != (method == "events.unsubscribe"))
          throw Error("INVALID_ARGUMENT", "Subscription does not belong to this session or method");
        session->subscriptions.erase(found); session->events.erase(subscription); result = true;
      } else if (method == "stream.open") {
        shape(params, {"name"});
        if (session->consumers.size() >= 64) throw Error("QUEUE_LIMIT", "Too many stream consumers");
        auto info = streams_.attach_external(name(params, "name"), session->id);
        const auto consumer = session->identity + ":c" + std::to_string(++session->sequence);
        const auto ticket = info.at("token"); const auto endpoint = info.at("url");
        session->consumers.emplace(consumer, ticket.get<std::string>()); info.erase("token"); info.erase("url");
        result = {{"consumerId", consumer}, {"endpoint", endpoint}, {"ticket", ticket}, {"info", info}};
      } else if (method == "stream.close") {
        shape(params, {"consumerId"}); const auto consumer = identifier(params, "consumerId");
        const auto found = session->consumers.find(consumer);
        if (found == session->consumers.end()) throw Error("INVALID_ARGUMENT", "Consumer does not belong to this session");
        streams_.detach_external(session->id, found->second); session->consumers.erase(found); result = true;
      } else throw Error("UNKNOWN_METHOD", "Unknown External Protocol method");
      respond({{"result", std::move(result)}});
    } catch (const Error& error) {
      Json value{{"code", error.code}, {"message", error.what()}};
      if (!error.details.is_null()) value["details"] = error.details;
      session->connection->send({{"type", "response"}, {"id", id}, {"error", value}}, close);
    } catch (const std::exception&) {
      session->connection->send({{"type", "response"}, {"id", id}, {"error", {{"code", "INVALID_ARGUMENT"}, {"message", "Invalid request parameters"}}}}, close);
    }
    return true;
  }
  return false;
}
}
