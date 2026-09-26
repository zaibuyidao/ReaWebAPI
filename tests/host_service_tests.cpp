#include "runtime/host_service.hpp"
#include <iostream>
#include <thread>
using namespace reaweb;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
struct Extension {
  ServiceRegistry* registry;
  uint64_t pending = 0;
  int sends = 0, cancellations = 0;
  static int request(void* user, uint64_t service, uint64_t id, int window, const char* method, const char* payload) {
    auto& e = *static_cast<Extension*>(user);
    if (std::string(method) == "ping") return e.registry->complete(service, id, "\"pong\"", REAWEB_OK, nullptr);
    if (std::string(method) == "pending") { e.pending = id; return REAWEB_OK; }
    if (std::string(method) == "message") { ++e.sends; return e.registry->emit(service, window, "changed", payload); }
    return REAWEB_METHOD_NOT_FOUND;
  }
  static void cancel(void* user, uint64_t) { ++static_cast<Extension*>(user)->cancellations; }
};
int main() {
  try {
    Json events = Json::array(), result;
    ServiceRegistry registry([&](auto handle, const auto& service, int window, const auto& name, Json data) {
      events.push_back({handle, service, window, name, data});
    });
    Extension extension{&registry};
    ReaWeb_ServiceCallbacks cb{sizeof(cb), REAWEB_SERVICE_ABI, &extension, Extension::request, Extension::cancel};
    uint64_t handle = 0, duplicate = 0;
    CHECK(registry.add("test", &cb, &handle) == REAWEB_OK);
    CHECK(registry.add("test", &cb, &duplicate) == REAWEB_SERVICE_EXISTS);
    CHECK(registry.add("bad/name", &cb, &duplicate) == REAWEB_INVALID_ARGUMENT);
    int wrong = 0;
    std::thread([&] { wrong = registry.remove(handle); }).join();
    CHECK(wrong == REAWEB_MAIN_THREAD_REQUIRED);
    auto reply = [&](Json value) { result = value; };
    registry.call("test", "ping", nullptr, 1, reply); registry.tick();
    CHECK(result["result"] == "pong");
    registry.call("test", "message", {{"n", 1}}, 1, {}); registry.tick();
    CHECK(extension.sends == 1 && events.back()[3] == "changed" && events.back()[4]["n"] == 1);
    registry.call("test", "missing", nullptr, 1, reply); registry.tick();
    CHECK(result["error"]["code"] == "METHOD_NOT_FOUND");
    registry.call("test", "pending", nullptr, 1, reply);
    const auto request = extension.pending;
    std::thread([&] { CHECK(registry.complete(handle, request, "{\"worker\":true}", 0, nullptr) == REAWEB_OK); }).join();
    CHECK(registry.complete(handle, request, "null", 0, nullptr) == REAWEB_REQUEST_GONE);
    registry.tick(); CHECK(result["result"]["worker"] == true);
    registry.call("test", "pending", nullptr, 1, reply);
    registry.tick(ServiceRegistry::Clock::now() + std::chrono::seconds(31));
    CHECK(result["error"]["code"] == "TIMEOUT" && extension.cancellations == 1);
    registry.call("test", "pending", nullptr, 1, reply); result = nullptr;
    registry.cancel_window(1); registry.tick();
    CHECK(result.is_null() && extension.cancellations == 2);
    CHECK(registry.complete(handle, extension.pending, "null", 0, nullptr) == REAWEB_REQUEST_GONE);
    registry.call("test", "pending", nullptr, 2, reply);
    CHECK(registry.remove(handle) == REAWEB_OK);
    CHECK(result["error"]["code"] == "EXTENSION_UNLOADED" && events.back()[3] == "unloaded");
    CHECK(registry.remove(handle) == REAWEB_OK);
    CHECK(registry.emit(handle, 0, "changed", "null") == REAWEB_EXTENSION_UNLOADED);
    CHECK(registry.complete(handle, extension.pending, "null", 0, nullptr) == REAWEB_EXTENSION_UNLOADED);
    bool missing = false;
    try { registry.call("test", "ping", nullptr, 1, reply); } catch (const Error& error) { missing = error.code == "SERVICE_NOT_FOUND"; }
    CHECK(missing);
    CHECK(registry.add("test", &cb, &duplicate) == REAWEB_OK && duplicate != handle);
    CHECK(registry.emit(duplicate, 0, "unloaded", "null") == REAWEB_INVALID_ARGUMENT);
    CHECK(registry.emit(duplicate, 0, "changed", "invalid json") == REAWEB_INVALID_ARGUMENT);
    for (int i = 0; i < 2048; ++i) CHECK(registry.emit(duplicate, 0, "changed", "null") == REAWEB_OK);
    CHECK(registry.emit(duplicate, 0, "changed", "null") == REAWEB_QUEUE_LIMIT);
    registry.remove(duplicate);
    std::cout << "Host Service lifecycle, worker completion, limits and errors passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
