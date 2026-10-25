#include "runtime/runtime.hpp"
#include "host_adapter.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
using namespace reaweb;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("Check failed: " #condition); } while (false)
namespace {
struct FakeWindow : Window {
  WindowOptions options;
  bool is_closed = false;
  std::vector<Json> responses;
  int sequence = 0;
  explicit FakeWindow(WindowOptions value) : options(std::move(value)) {}
  void evaluate(const std::string& script) override {
    const auto start = script.find('(') + 1;
    auto message = Json::parse(script.substr(start, script.size() - start - 2));
    if (!message.value("started", false)) responses.push_back(std::move(message));
  }
  void devtools() override {}
  bool closed() const override { return is_closed; }
  void* native_handle() const override { return const_cast<FakeWindow*>(this); }
  int send(const std::string& method, Json args = Json::array()) {
    Json request{{"id", ++sequence}, {"document", "first"}, {"method", method}, {"args", args}};
    options.on_message(request.dump());
    return sequence;
  }
  Json response(int id) {
    for (const auto& item : responses) if (item.value("id", Json()) == id) return item;
    return nullptr;
  }
  // Frame events carry no id, so they are counted separately.
  int frames() const {
    int count = 0;
    for (const auto& item : responses) if (item.value("event", "") == "frame") ++count;
    return count;
  }
  Json last_frame() const {
    for (auto it = responses.rbegin(); it != responses.rend(); ++it) if (it->value("event", "") == "frame") return (*it)["data"];
    return nullptr;
  }
};
std::vector<std::weak_ptr<FakeWindow>> windows;
struct FakePlatform : Platform {
  std::shared_ptr<Window> open(WindowOptions options) override {
    auto window = std::make_shared<FakeWindow>(std::move(options));
    windows.push_back(window);
    return window;
  }
};
void until(Runtime& runtime, const std::function<bool()>& done) {
  const auto deadline = Clock::now() + std::chrono::seconds(4);
  do {
    runtime.tick();
    if (done()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (Clock::now() < deadline);
  throw std::runtime_error("Timed out waiting for a frame");
}
Json result(Runtime& runtime, FakeWindow& window, int request) {
  until(runtime, [&] { return !window.response(request).is_null(); });
  return window.response(request);
}
int playing = 1;
double position = 1.5, tempo = 120.0, cursor = 2.5;
// Runtime copies the Host it is given, so a later test cannot swap the resolver.
// This flag is how the test takes the REAPER functions away instead.
bool host_reads_available = true;
const char* content = "{\"track\":{\"volume\":0.5}}";
}
namespace reaweb {
std::unique_ptr<Platform> make_platform(const fs::path&) { return std::make_unique<FakePlatform>(); }
}
int main() {
  int step = 0;
  try {
    auto root = fs::current_path() / ("frame-test-" + std::to_string(Clock::now().time_since_epoch().count()));
    auto entry = root / "Scripts" / "Tool" / "index.html";
    fs::create_directories(entry.parent_path()); std::ofstream(entry) << "<html></html>";
    std::ofstream(entry.parent_path() / "app.json") << Json{{"id", "frame-" + state_key(entry.parent_path().u8string(), 0)}};
    int storage = 0, changes = 0, track = 0;
    void* current = &storage;
    Guid guid{};
    adapter::Host host;
    host.current_project = [&] { return current; };
    host.count_tracks = [](void*) { return 1; };
    host.get_track = [](void*, int) -> void* { return nullptr; };
    host.project_generation = [] { return uint64_t{0}; };
    host.change_count = [&](void*) { return changes; };
    host.count_selected_tracks = [](void*) { return 1; };
    host.get_selected_track = [&](void*, int) -> void* { return &track; };
    host.valid_track = [&](void*, void* p) { return p == &track; };
    host.track_guid = [&](void*) { return guid; };
    host.track_count = [] { return 1; };
    host.track_identity = [](int) { return std::string("track"); };
    adapter::connect(host);
    // The frame channel reads REAPER directly, so these stubs are its whole
    // contract. Returning null for one of them must degrade a field, not a frame.
    host.native_function = [](const char* name) -> void* {
      if (!host_reads_available) {
        // These four are the frame channel's own reads. Withdraw them and the
        // frame must still arrive, just without those fields.
        if (!std::strcmp(name, "GetPlayState") || !std::strcmp(name, "GetPlayPosition") ||
            !std::strcmp(name, "Master_GetTempo") || !std::strcmp(name, "GetCursorPosition")) return nullptr;
        return adapter::resolve(name);
      }
      if (!std::strcmp(name, "GetPlayState")) return reinterpret_cast<void*>(+[] { return playing; });
      if (!std::strcmp(name, "GetPlayPosition")) return reinterpret_cast<void*>(+[] { return position; });
      if (!std::strcmp(name, "Master_GetTempo")) return reinterpret_cast<void*>(+[] { return tempo; });
      if (!std::strcmp(name, "GetCursorPosition")) return reinterpret_cast<void*>(+[] { return cursor; });
      return adapter::resolve(name);
    };
    Runtime runtime(host, root, [](const std::string&) {});
    const auto id = runtime.open("Tool/index.html");
    auto window = windows.back().lock();
    result(runtime, *window, window->send("__reawebHello", {1}));
    step = 1;
    // 1. Nothing is pushed before the page subscribes, then frames arrive on the
    // extension's own cadence rather than on every tick.
    CHECK(result(runtime, *window, window->send("ReaWeb_Subscribe", {"frame"}))["result"].is_null());
    const auto before = window->frames();
    CHECK(before == 0);
    const auto started = Clock::now();
    until(runtime, [&] { return Clock::now() - started > std::chrono::milliseconds(200); });
    const auto pushed = window->frames() - before;
    // The timer paces this, not frame_interval, so the count follows the timer.
    // The bounds are wide on purpose: the claim is that frames arrive on the
    // timer rather than once per tick, not that this machine's timer is precise.
    CHECK(pushed >= 3 && pushed <= 40);
    step = 2;
    // 2. The frame carries values the extension read itself.
    const auto frame = window->last_frame();
    CHECK(frame["playing"] == playing);
    CHECK(frame["tempo"] == tempo);
    CHECK(frame["position"].get<double>() == position);
    CHECK(frame["cursor"].get<double>() == cursor);
    step = 3;
    // 3. No Lua content yet, so there is no content field and no age.
    CHECK(!frame.contains("lua") && !frame.contains("luaAgeMs"));
    step = 4;
    // 4. Content written by Lua appears in the next frame, verbatim.
    CHECK(runtime.set_frame(id, content));
    until(runtime, [&] { return window->last_frame().contains("lua"); });
    CHECK(window->last_frame()["lua"] == content);
    CHECK(window->last_frame().contains("luaAgeMs"));
    step = 5;
    // 5. Oversized content is refused and leaves the previous content in place.
    bool refused = false;
    try { runtime.set_frame(id, std::string(host_message_limit + 1, 'x')); }
    catch (const Error& error) { refused = error.code == "MESSAGE_LIMIT"; }
    CHECK(refused);
    CHECK(window->last_frame()["lua"] == content);
    step = 6;
    // 6. An embedded NUL is refused, so the page never receives truncated text.
    refused = false;
    try { runtime.set_frame(id, std::string("a\0b", 3)); }
    catch (const Error& error) { refused = error.code == "INVALID_ARGUMENT"; }
    CHECK(refused);
    step = 7;
    // 7. Unknown windows and invalid UTF-8 are refused rather than silently kept.
    refused = false;
    try { runtime.set_frame(id + 1000, "{}"); }
    catch (const Error& error) { refused = error.code == "WINDOW_CLOSED"; }
    CHECK(refused);
    refused = false;
    try { runtime.set_frame(id, std::string("\xff", 1)); }
    catch (const Error& error) { refused = error.code == "INVALID_ARGUMENT"; }
    CHECK(refused);
    step = 8;
    // 8. A frame still arrives when every host read is unavailable, so a missing
    // REAPER function degrades a field instead of stopping the channel.
    host_reads_available = false;
    const auto before_missing = window->frames();
    until(runtime, [&] { return window->frames() > before_missing; });
    const auto degraded = window->last_frame();
    CHECK(!degraded.contains("playing") && !degraded.contains("tempo"));
    CHECK(degraded.contains("lua"));
    step = 9;
    // 9. Diagnostics report the channel, which is what the page and the user read.
    const auto diagnostics = runtime.diagnostics(id);
    CHECK(diagnostics["frames"]["pushed"].get<uint64_t>() > 0);
    CHECK(diagnostics["frames"]["intervalMs"] == 8);
    CHECK(diagnostics["frames"]["luaWrites"].get<uint64_t>() >= 1);
    CHECK(!diagnostics["frames"]["luaFrameAgeMs"].is_null());
    step = 10;
    // 10. A replacement document does not inherit the previous document's content.
    // Without this, the new page shows the old page's numbers until Lua runs again.
    window->options.on_navigation();
    CHECK(runtime.diagnostics(id)["frames"]["luaFrameAgeMs"].is_null());
    result(runtime, *window, window->send("__reawebHello", {1}));
    result(runtime, *window, window->send("ReaWeb_Subscribe", {"frame"}));
    const auto before_navigation = window->frames();
    until(runtime, [&] { return window->frames() > before_navigation; });
    CHECK(!window->last_frame().contains("lua"));
    CHECK(!window->last_frame().contains("luaAgeMs"));
    step = 11;
    // 11. A closed window stops pushing instead of throwing.
    window->is_closed = true;
    const auto before_closed = window->frames();
    for (int n = 0; n < 5; ++n) runtime.tick();
    CHECK(window->frames() == before_closed);
    std::cout << "frame tests passed\n";
    fs::remove_all(root);
  } catch (const std::exception& error) {
    std::cerr << "frame tests failed after step " << step << ": " << error.what() << "\n"; return 1;
  }
  return 0;
}
