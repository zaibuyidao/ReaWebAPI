#include "runtime/host_service.hpp"
#include <cstring>

namespace reaweb {
namespace {
bool valid_name(const char* value) {
  if (!value || !*value || std::strlen(value) > 128) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return false;
  return true;
}
Json parse(const char* value) {
  if (!value || std::strlen(value) > host_message_limit) throw Error("INVALID_ARGUMENT", "Expected JSON up to 1 MiB");
  return Json::parse(value, [](int depth, Json::parse_event_t, Json&) {
    if (depth > 64) throw Error("INVALID_ARGUMENT", "JSON nesting exceeds 64 levels");
    return true;
  });
}
Json failure(int status, const char* message = nullptr) {
  return {{"error", {{"code", ServiceRegistry::code(status)}, {"message", message && *message ? message : ServiceRegistry::code(status)}}}};
}
}
const char* ServiceRegistry::code(int status) {
  switch (status) {
    case REAWEB_OK: return "OK";
    case REAWEB_SERVICE_NOT_FOUND: return "SERVICE_NOT_FOUND";
    case REAWEB_METHOD_NOT_FOUND: return "METHOD_NOT_FOUND";
    case REAWEB_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case REAWEB_EXTENSION_UNLOADED: return "EXTENSION_UNLOADED";
    case REAWEB_MAIN_THREAD_REQUIRED: return "MAIN_THREAD_REQUIRED";
    case REAWEB_TIMEOUT: return "TIMEOUT";
    case REAWEB_SERVICE_EXISTS: return "SERVICE_EXISTS";
    case REAWEB_QUEUE_LIMIT: return "QUEUE_LIMIT";
    case REAWEB_REQUEST_GONE: return "REQUEST_GONE";
    default: return "SERVICE_ERROR";
  }
}
ServiceRegistry::ServiceRegistry(Event event) : event_(std::move(event)) {}
int ServiceRegistry::add(const char* name, const ReaWeb_ServiceCallbacks* cb, uint64_t* handle) {
  if (std::this_thread::get_id() != main_thread_) return REAWEB_MAIN_THREAD_REQUIRED;
  if (!valid_name(name) || !cb || cb->size < sizeof(*cb) || cb->abi_version != REAWEB_SERVICE_ABI || !cb->on_request || !handle)
    return REAWEB_INVALID_ARGUMENT;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& service : services_) if (service.second.name == name) return REAWEB_SERVICE_EXISTS;
  if (services_.size() >= 256) return REAWEB_QUEUE_LIMIT;
  *handle = ++next_service_;
  services_.emplace(*handle, Service{name, *cb, {}});
  return REAWEB_OK;
}
uint64_t ServiceRegistry::lookup(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& service : services_) if (service.second.name == name) return service.first;
  throw Error("SERVICE_NOT_FOUND", "Host Service is not registered: " + name);
}
int ServiceRegistry::set_input(uint64_t handle, const char* method) {
  if (std::this_thread::get_id() != main_thread_) return REAWEB_MAIN_THREAD_REQUIRED;
  if (!valid_name(method)) return REAWEB_INVALID_ARGUMENT;
  auto it = services_.find(handle);
  if (it == services_.end()) return REAWEB_SERVICE_NOT_FOUND;
  if (it->second.inputs.size() >= 8) return REAWEB_QUEUE_LIMIT;
  it->second.inputs.insert(method);
  return REAWEB_OK;
}
bool ServiceRegistry::is_input(const std::string& name, const std::string& method) const {
  if (std::this_thread::get_id() != main_thread_) return false;
  for (const auto& item : services_) if (item.second.name == name) return item.second.inputs.count(method) != 0;
  return false;
}
bool ServiceRegistry::contains(uint64_t handle) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return services_.count(handle) != 0;
}
int ServiceRegistry::set_shutdown(uint64_t handle, ReaWeb_ServiceShutdown callback) {
  if (std::this_thread::get_id() != main_thread_) return REAWEB_MAIN_THREAD_REQUIRED;
  auto it = services_.find(handle);
  if (it == services_.end()) return REAWEB_SERVICE_NOT_FOUND;
  it->second.shutdown = callback;
  return REAWEB_OK;
}
void ServiceRegistry::shutdown() {
  while (!services_.empty()) remove(services_.begin()->first);
}
void ServiceRegistry::cancel(int window, uint64_t service, int status) {
  struct Cancelled { uint64_t id; Pending pending; };
  std::vector<Cancelled> cancelled;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = pending_.begin(); it != pending_.end();) {
      if ((window && it->second.window == window) || (service && it->second.service == service)) {
        cancelled.push_back({it->first, std::move(it->second)});
        it = pending_.erase(it);
      } else ++it;
    }
  }
  for (auto& item : cancelled) {
    ReaWeb_ServiceCallbacks callbacks{};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto source = services_.find(item.pending.service);
      if (source != services_.end()) callbacks = source->second.callbacks;
    }
    if (callbacks.on_cancel) try { callbacks.on_cancel(callbacks.user_data, item.id); } catch (...) {}
    if (status) item.pending.reply(failure(status));
  }
}
int ServiceRegistry::remove(uint64_t handle) {
  if (std::this_thread::get_id() != main_thread_) return REAWEB_MAIN_THREAD_REQUIRED;
  Service service;
  std::vector<std::pair<uint64_t, Pending>> cancelled;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = services_.find(handle);
    if (it == services_.end()) return REAWEB_OK;
    service = it->second;
    services_.erase(it);
    for (auto p = pending_.begin(); p != pending_.end();) {
      if (p->second.service == handle) { cancelled.emplace_back(p->first, std::move(p->second)); p = pending_.erase(p); }
      else ++p;
    }
    for (auto o = output_.begin(); o != output_.end();) {
      if (o->service == handle) { output_bytes_ -= o->bytes; o = output_.erase(o); } else ++o;
    }
  }
  if (service.shutdown) try { service.shutdown(service.callbacks.user_data); } catch (...) {}
  for (auto& item : cancelled) {
    if (service.callbacks.on_cancel) try { service.callbacks.on_cancel(service.callbacks.user_data, item.first); } catch (...) {}
    item.second.reply(failure(REAWEB_EXTENSION_UNLOADED));
  }
  event_(handle, service.name, 0, "unloaded", {{"code", "EXTENSION_UNLOADED"}});
  return REAWEB_OK;
}
void ServiceRegistry::call(const std::string& name, const std::string& method, const Json& payload, int window, Reply reply) {
  if (std::this_thread::get_id() != main_thread_) throw Error("MAIN_THREAD_REQUIRED", "Service dispatch requires the main thread");
  if (!valid_name(name.c_str()) || name.find('\0') != std::string::npos || !valid_name(method.c_str()) || method.find('\0') != std::string::npos)
    throw Error("INVALID_ARGUMENT", "Invalid service or method name");
  const auto text = payload.dump();
  if (text.size() > host_message_limit) throw Error("INVALID_ARGUMENT", "Service payload exceeds 1 MiB");
  if (!reply && is_input(name, method)) {
    // Registration and input dispatch share the main thread. Worker completions
    // never mutate services, so input does not contend on the completion lock.
    for (const auto& item : services_) if (item.second.name == name) {
      const auto cb = item.second.callbacks;
      int status;
      try { status = cb.on_request(cb.user_data, item.first, 0, window, method.c_str(), text.c_str()); }
      catch (...) { status = REAWEB_SERVICE_ERROR; }
      if (status != REAWEB_OK) throw Error(code(status), "Host Service input failed: " + method);
      return;
    }
  }
  const auto handle = lookup(name);
  ReaWeb_ServiceCallbacks cb;
  uint64_t request = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cb = services_.at(handle).callbacks;
    if (reply) {
      if (pending_.size() >= 1024) throw Error("QUEUE_LIMIT", "Too many pending service calls");
      request = ++next_request_;
      pending_.emplace(request, Pending{handle, window, std::move(reply), Clock::now() + std::chrono::seconds(30)});
    }
  }
  int status;
  try { status = cb.on_request(cb.user_data, handle, request, window, method.c_str(), text.c_str()); }
  catch (...) { status = REAWEB_SERVICE_ERROR; }
  if (status != REAWEB_OK) {
    if (!request) throw Error(code(status), "Host Service send failed: " + method);
    Reply failed;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = pending_.find(request);
      if (it != pending_.end() && !it->second.completed) { failed = std::move(it->second.reply); pending_.erase(it); }
    }
    if (failed) failed(failure(status));
  }
}
int ServiceRegistry::complete(uint64_t handle, uint64_t request, const char* json, int status, const char* message) {
  try {
    if (message && std::strlen(message) > host_message_limit) return REAWEB_INVALID_ARGUMENT;
    auto data = status == REAWEB_OK ? Json{{"result", parse(json)}} : failure(status, message);
    const auto bytes = data.dump().size();
    if (bytes > host_message_limit) return REAWEB_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!services_.count(handle)) return REAWEB_EXTENSION_UNLOADED;
    auto it = pending_.find(request);
    if (it == pending_.end() || it->second.service != handle || it->second.completed) return REAWEB_REQUEST_GONE;
    if (output_.size() >= 2048 || output_bytes_ + bytes > host_queue_bytes) return REAWEB_QUEUE_LIMIT;
    it->second.completed = true;
    output_.push_back({handle, request, it->second.window, {}, std::move(data), bytes}); output_bytes_ += bytes;
    return REAWEB_OK;
  } catch (...) { return REAWEB_INVALID_ARGUMENT; }
}
int ServiceRegistry::emit(uint64_t handle, int window, const char* event, const char* json) {
  if (window < 0 || !valid_name(event) || std::strcmp(event, "unloaded") == 0) return REAWEB_INVALID_ARGUMENT;
  try {
    auto data = parse(json);
    const auto bytes = data.dump().size();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!services_.count(handle)) return REAWEB_EXTENSION_UNLOADED;
    if (output_.size() >= 2048 || output_bytes_ + bytes > host_queue_bytes) return REAWEB_QUEUE_LIMIT;
    output_.push_back({handle, 0, window, event, std::move(data), bytes}); output_bytes_ += bytes;
    return REAWEB_OK;
  } catch (...) { return REAWEB_INVALID_ARGUMENT; }
}
void ServiceRegistry::cancel_window(int window) {
  cancel(window, 0, 0);
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = output_.begin(); it != output_.end();) {
    if (it->window == window) { output_bytes_ -= it->bytes; it = output_.erase(it); } else ++it;
  }
}
void ServiceRegistry::tick(Clock::time_point now, Clock::time_point deadline) {
  for (int n = 0; n < 128 && Clock::now() < deadline; ++n) {
    Output output; Reply reply; std::string name;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (output_.empty()) break;
      output = std::move(output_.front()); output_.pop_front(); output_bytes_ -= output.bytes;
      auto source = services_.find(output.service);
      if (source == services_.end()) continue;
      name = source->second.name;
      if (output.request) {
        auto p = pending_.find(output.request);
        if (p == pending_.end()) continue;
        reply = std::move(p->second.reply); pending_.erase(p);
      }
    }
    if (reply) reply(std::move(output.data));
    else event_(output.service, name, output.window, output.event, std::move(output.data));
  }
  // Remove before calling extension code, which may re-enter the registry.
  while (Clock::now() < deadline) {
    uint64_t id = 0; Pending pending; ReaWeb_ServiceCallbacks cb{};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = pending_.begin(); it != pending_.end(); ++it) if (it->second.deadline <= now && !it->second.completed) {
        id = it->first; pending = std::move(it->second); cb = services_.at(pending.service).callbacks; pending_.erase(it); break;
      }
    }
    if (!id) break;
    if (cb.on_cancel) try { cb.on_cancel(cb.user_data, id); } catch (...) {}
    pending.reply(failure(REAWEB_TIMEOUT));
  }
}
}
