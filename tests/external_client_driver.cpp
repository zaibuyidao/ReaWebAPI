#include "runtime/runtime.hpp"
#include "host_adapter.hpp"
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

using namespace reaweb;
#define CHECK(c) do { if (!(c)) throw std::runtime_error("Check failed: " #c); } while (false)
namespace {
struct TestWindow : Window {
  WindowOptions options;
  std::vector<Json> received;
  explicit TestWindow(WindowOptions value) : options(std::move(value)) {}
  void evaluate(const std::string& script) override {
    auto start = script.find('(') + 1;
    received.push_back(Json::parse(script.substr(start, script.size() - start - 2)));
  }
  void devtools() override {}
  bool closed() const override { return false; }
  void set_icon(const std::vector<IconBitmap>&) override {}
  void clear_icon() override {}
};
std::shared_ptr<TestWindow> window;
int platform_count = 0;
struct TestPlatform : Platform {
  std::shared_ptr<Window> open(WindowOptions options) override { return window = std::make_shared<TestWindow>(std::move(options)); }
};
int sequence = 0;
Json web_call(Runtime& runtime, const std::string& method, Json args = Json::array()) {
  const auto id = ++sequence;
  window->options.on_message(Json{{"id", id}, {"document", "legacy"}, {"method", method}, {"args", args}}.dump());
  for (int i = 0; i < 2000; ++i) {
    runtime.tick();
    for (const auto& reply : window->received) if (reply.value("id", Json()) == id && !reply.value("started", false)) return reply;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("Legacy response timed out");
}
}
namespace reaweb { std::unique_ptr<Platform> make_platform(const fs::path&) { ++platform_count; return std::make_unique<TestPlatform>(); } }
int main(int argc, char** argv) {
  try {
    CHECK(argc == 3);
    auto root = fs::u8path(argv[2]); fs::create_directories(root / "Scripts" / "Test");
    std::ofstream(root / "Scripts" / "Test" / "index.html") << "<html></html>";
    std::ofstream(root / "Scripts" / "Test" / "app.json") << R"({"id":"external-coexistence-test"})";
    ExternalSettings settings{true, uint16_t(std::stoi(argv[1])), external_token()};
    CHECK(!read_external_settings(root / "ReaWebAPI.ini").enabled);
    std::ofstream(root / "ReaWebAPI.ini") << "[ReaWebAPI]\nWebViewColorProfile=sRGB\n";
    write_external_settings(root / "ReaWebAPI.ini", settings);
    auto persisted = read_external_settings(root / "ReaWebAPI.ini");
    CHECK(persisted.enabled && persisted.port == settings.port && persisted.token == settings.token);
    int project = 0, track = 0, cancelled = 0, window_id = 0;
    uint64_t generation = 0, pending = 0, service = 0, stream = 0;
    const auto main_thread = std::this_thread::get_id();
    adapter::Host host;
    host.current_project = [&]() -> void* { CHECK(std::this_thread::get_id() == main_thread); return &project; };
    host.version = [] { return "7.external-test"; };
    host.count_tracks = [](void*) { return 1; };
    host.project_name = [](void*) { return std::string("Test project"); };
    host.play_state = [] { return 0; };
    host.get_track = [&](void*, int index) -> void* { return index == 0 ? &track : nullptr; };
    host.count_selected_tracks = [](void*) { return 1; };
    host.get_selected_track = host.get_track;
    host.valid_track = [&](void*, void* value) { return value == &track; };
    host.track_guid = [](void*) { return Guid{}; };
    host.track_name = [](void*) { return std::string(u8"Track 测试"); };
    host.get_track_value = [](void*, const std::string&) { return 0.5; };
    host.set_track_value = [](void*, const std::string&, double value) { return value != 9; };
    host.track_color = [](void*) { return 0; };
    host.count_markers = [](void*) { return Json::array({0, 0, 0}); };
    host.change_count = [](void*) { return 0; };
    host.project_generation = [&] { return generation; };
    adapter::connect(host);
    host.native_function = [](const char* name) -> void* {
      if (std::string(name) == "EnumProjects") return reinterpret_cast<void*>(+[](int index, char* path, int capacity) -> void* {
        if (path && capacity) path[0] = '\0'; return index <= 0 ? adapter::host->current_project() : nullptr;
      });
      if (std::string(name) == "IsProjectDirty") return reinterpret_cast<void*>(+[](void*) { return 0; });
      return adapter::resolve(name);
    };
    Runtime runtime(host, root, [](const std::string& error) { std::cerr << error << '\n'; });
    CHECK(runtime.external_port() == 0 && platform_count == 0);
    runtime.configure_external(settings);
    CHECK(platform_count == 0);
    struct Context { Runtime* runtime; uint64_t* pending; int* cancelled; std::thread::id main; } context{&runtime, &pending, &cancelled, main_thread};
    ReaWeb_ServiceCallbacks callbacks{sizeof(callbacks), REAWEB_SERVICE_ABI, &context,
      [](void* pointer, uint64_t handle, uint64_t request, int window, const char* method, const char* payload) {
        auto& c = *static_cast<Context*>(pointer); CHECK(std::this_thread::get_id() == c.main);
        if (std::string(method) == "pending") { *c.pending = request; return int(REAWEB_OK); }
        if (std::string(method) == "echo" || std::string(method) == "input") {
          if (request) return c.runtime->services().complete(handle, request, payload, REAWEB_OK, nullptr);
          return int(REAWEB_OK);
        }
        if (std::string(method) == "consumer") {
          auto value = Json{{"windowId", window}}.dump();
          return c.runtime->services().complete(handle, request, value.c_str(), REAWEB_OK, nullptr);
        }
        return int(REAWEB_METHOD_NOT_FOUND);
      }, [](void* pointer, uint64_t) { ++*static_cast<Context*>(pointer)->cancelled; }};
    CHECK(runtime.services().add("test", &callbacks, &service) == REAWEB_OK);
    CHECK(runtime.services().set_input(service, "input") == REAWEB_OK);
    ReaWeb_StreamDesc desc{}; desc.size = sizeof(desc); desc.abi_version = REAWEB_STREAM_ABI;
    desc.format = REAWEB_BYTES; desc.capacity = 4; desc.max_bytes = 64; desc.owner = service;
    CHECK(runtime.streams().create(REAWEB_BINARY, "test.binary", &desc, &stream) == REAWEB_OK);
    std::cout << Json{{"port", runtime.external_port()}, {"token", settings.token}}.dump() << std::endl;
    std::mutex mutex; std::deque<Json> commands;
    std::thread input([&] {
      for (std::string line; std::getline(std::cin, line);) {
        auto command = Json::parse(line); const bool stop = command.at("cmd") == "stop";
        { std::lock_guard<std::mutex> lock(mutex); commands.push_back(std::move(command)); }
        if (stop) return;
      }
      std::lock_guard<std::mutex> lock(mutex); commands.push_back({{"cmd", "stop"}});
    });
    bool stop = false;
    while (!stop) {
      runtime.tick();
      Json command;
      { std::lock_guard<std::mutex> lock(mutex); if (!commands.empty()) { command = std::move(commands.front()); commands.pop_front(); } }
      if (!command.is_null()) {
        Json result;
        try {
          const auto cmd = command.at("cmd").get<std::string>();
          if (cmd == "web") {
            if (!window) {
              window_id = runtime.open("Test/index.html"); CHECK(web_call(runtime, "__reawebHello", {1}).contains("result"));
              CHECK(web_call(runtime, "ReaWeb_ServiceSubscribe", {"test", "update"}).contains("result"));
            }
            result = web_call(runtime, command.value("method", "CountTracks"), command.value("args", Json::array({0})));
          } else if (cmd == "stats") result = {{"cancelled", cancelled}, {"streams", runtime.streams().info()}, {"platforms", platform_count}};
          else if (cmd == "rotate") { settings.token = external_token(); runtime.configure_external(settings); result = {{"token", settings.token}}; }
          else if (cmd == "enabled") { settings.enabled = command.at("value"); runtime.configure_external(settings); result = true; }
          else if (cmd == "port") { auto candidate = settings; candidate.port = command.at("value"); runtime.configure_external(candidate); settings = candidate; result = true; }
          else if (cmd == "generation") { ++generation; result = true; }
          else if (cmd == "publish") { unsigned char bytes[] = {0, 255, 128, 1}; result = runtime.streams().publish(REAWEB_BINARY, stream, bytes, sizeof(bytes), 1, 1.25); }
          else if (cmd == "emit") { result = runtime.services().emit(service, command.value("target", false) ? window_id : 0, "update", "{\"value\":1}"); }
          else if (cmd == "complete") result = runtime.services().complete(service, pending, "42", REAWEB_OK, nullptr);
          else if (cmd == "unload") result = runtime.services().remove(service);
          else if (cmd == "stop") { result = true; stop = true; }
          else throw std::runtime_error("Unknown driver command");
          std::cout << Json{{"result", result}}.dump() << std::endl;
        } catch (const Error& error) { std::cout << Json{{"error", error.code}}.dump() << std::endl; }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    input.join();
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
}
