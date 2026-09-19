#include "runtime.hpp"
#include "host_adapter.hpp"
#include <fstream>
#include <algorithm>
#include <iostream>
#include <set>
using namespace reaweb;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("Check failed: " #condition); } while (false)
namespace {
int platform_count = 0;
std::vector<fs::path> profiles;
std::string waiting_on;
struct FakeWindow : Window {
  WindowOptions options;
  bool is_closed = false;
  std::vector<Json> responses;
  int sequence = 0;
  std::string document = "first";
  Json bounds = {{"x", 40}, {"y", 50}, {"width", 860}, {"height", 640}, {"maximized", false}};
  explicit FakeWindow(WindowOptions value) : options(std::move(value)) {}
  void evaluate(const std::string& script) override {
    const auto start = script.find('(') + 1;
    auto message = Json::parse(script.substr(start, script.size() - start - 2));
    if (!message.value("started", false)) responses.push_back(std::move(message));
  }
  void devtools() override {}
  bool closed() const override { return is_closed; }
  void* native_handle() const override { return const_cast<FakeWindow*>(this); }
  Json placement() const override { return bounds; }
  void restore_placement(const Json& value) override { bounds = value; }
  int send(const std::string& method, Json args = Json::array(), uint64_t epoch = 0, int64_t expires = 0) {
    waiting_on = method + " " + args.dump();
    Json request{{"id", ++sequence}, {"document", document}, {"method", method}, {"args", args}};
    if (epoch) request["project"] = epoch;
    if (expires) request["expiresAt"] = expires;
    options.on_message(request.dump());
    return sequence;
  }
  Json response(int id) {
    for (const auto& item : responses) if (item.value("id", Json()) == id) return item;
    return nullptr;
  }
};
std::vector<std::weak_ptr<FakeWindow>> windows;
struct FakePlatform : Platform {
  std::string clipboard;
  void desktop(const std::string& method, const Json& args, DesktopReply reply) override {
    if (method == "ReaWeb_ClipboardWriteText") { clipboard = args[0]; reply({{"result", true}}); }
    else if (method == "ReaWeb_ClipboardReadText") reply({{"result", clipboard}});
    else reply({{"result", true}});
  }
  std::shared_ptr<Window> open(WindowOptions options) override {
    auto window = std::make_shared<FakeWindow>(std::move(options));
    windows.push_back(window);
    return window;
  }
};
void until(Runtime& runtime, const std::function<bool()>& done) {
  auto deadline = Clock::now() + std::chrono::seconds(4);
  do {
    runtime.tick();
    if (done()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (Clock::now() < deadline);
  throw std::runtime_error("Timed out waiting for runtime after " + waiting_on);
}
Json result(Runtime& runtime, FakeWindow& window, int request) {
  until(runtime, [&] { return !window.response(request).is_null(); });
  return window.response(request);
}
}
namespace reaweb {
std::unique_ptr<Platform> make_platform(const fs::path& profile) { ++platform_count; profiles.push_back(profile); return std::make_unique<FakePlatform>(); }
}
int main() {
  try {
    auto root = fs::current_path() / ("runtime-test-" + std::to_string(Clock::now().time_since_epoch().count()));
    auto entry = root / "Scripts" / "Tool" / "index.html";
    fs::create_directories(entry.parent_path()); std::ofstream(entry) << "<html></html>";
    int storage = 0, other = 0, track = 0, calls = 0, changes = 0;
    void* current = &storage;
    Guid guid{};
    uint64_t generation = 0;
    std::vector<int> order;
    const auto main_thread = std::this_thread::get_id();
    adapter::Host host;
    host.current_project = [&] { CHECK(std::this_thread::get_id() == main_thread); return current; };
    host.count_tracks = [&](void*) { CHECK(std::this_thread::get_id() == main_thread); ++calls; return 3; };
    host.get_track = [&](void*, int index) -> void* { order.push_back(index); std::this_thread::sleep_for(std::chrono::milliseconds(1)); return nullptr; };
    host.project_generation = [&] { return generation; };
    host.change_count = [&](void*) { return changes; };
    host.count_selected_tracks = [](void*) { return 1; };
    host.get_selected_track = [&](void*, int) -> void* { return &track; };
    host.valid_track = [&](void*, void* p) { return p == &track; };
    host.track_guid = [&](void*) { return guid; };
    adapter::connect(host);
    std::set<void*> docked;
    std::vector<std::string> errors;
    DockApi docks{nullptr,
      [&](void* h, const std::string&, const std::string& ident) { CHECK(ident.find("ReaWebAPI:") == 0); docked.insert(h); },
      [&](void* h) { docked.erase(h); }, [&](void* h) { return docked.count(h) ? 3 : -1; }, [](void*) {}};
    {
      Runtime runtime(host, root, [&](const std::string& error) { errors.push_back(error); }, docks);
      const auto id = runtime.open("Tool/index.html"); auto first = windows.back().lock();
      CHECK(runtime.is_open(id) && !runtime.is_ready(id));
      CHECK(result(runtime, *first, first->send("CountTracks", {0}))["error"]["code"] == "DOCUMENT_STALE");
      CHECK(calls == 0);
      CHECK(result(runtime, *first, first->send("__reawebHello", {1}))["result"]["protocol"] == 1);
      CHECK(runtime.is_ready(id));
      const auto web = result(runtime, *first, first->send("ReaWeb_GetCapabilities"))["result"]["webRuntime"];
      CHECK(web["contract"] == 1 && web["mode"] == "app-http" && web["storageIsolation"] == "app-profile");
      CHECK(first->options.url == web["origin"].get<std::string>() + "/index.html");
      CHECK(profiles.back().parent_path().parent_path().filename() == "Apps");
      CHECK(result(runtime, *first, first->send("CountTracks", {0}))["result"] == 3 && calls == 1);
      CHECK(runtime.set_docked(id, true) && runtime.is_docked(id));
      CHECK(!runtime.set_docked(id, false));
      CHECK(runtime.captures_keyboard(first.get(), [](void*, void*) { return false; }));
      CHECK(result(runtime, *first, first->send("ReaWeb_SetKeyboardCapture", {false}))["result"] == false);
      CHECK(!runtime.captures_keyboard(first.get(), [](void*, void*) { return false; }));
      CHECK(result(runtime, *first, first->send("ReaWeb_SetTitle", {"Mixer"}))["result"] == true);
      CHECK(runtime.diagnostics(id)["window"]["title"] == "Mixer");
      result(runtime, *first, first->send("ReaWeb_Subscribe", {"selectionchange"}));
      until(runtime, [&] { for (const auto& r : first->responses) if (r.value("event", "") == "selectionchange") return true; return false; });
      auto before = first->responses.size(); ++guid[0]; ++changes;
      until(runtime, [&] { for (size_t i = before; i < first->responses.size(); ++i) if (first->responses[i].value("event", "") == "selectionchange") return true; return false; });
      const auto queued = first->send("CountTracks", Json::array({0}), 1);
      current = &other;
      CHECK(result(runtime, *first, queued)["error"]["code"] == "PROJECT_CHANGED");
      CHECK(calls == 1);
      CHECK(result(runtime, *first, first->send("CountTracks", Json::array({0}), 0, 1))["error"]["code"] == "REQUEST_EXPIRED");
      CHECK(calls == 1);
      const auto before_reload = first->send("CountTracks", {0});
      ++generation;
      CHECK(result(runtime, *first, before_reload)["error"]["code"] == "PROJECT_CHANGED");
      CHECK(runtime.diagnostics(id)["projectEpoch"] == 3 && calls == 1);
      const auto abandoned = first->send("CountTracks", {0});
      first->options.on_navigation(); first->document = "reloaded";
      result(runtime, *first, first->send("__reawebHello", {1}));
      CHECK(calls == 1 && first->response(abandoned).is_null());
      CHECK(runtime.captures_keyboard(first.get(), [](void*, void*) { return false; }));
      result(runtime, *first, first->send("ReaWebOpen", {"index.html"}));
      CHECK(windows.size() == 2 && platform_count == 1);
      const auto second_id = id + 1; auto second = windows.back().lock();
      result(runtime, *second, second->send("__reawebHello", {1}));
      for (int n = 0; n < 16; ++n) first->send("GetTrack", {0, 1});
      second->send("GetTrack", {0, 2});
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      until(runtime, [&] { return order.size() == 17; });
      CHECK(std::find(order.begin(), order.end(), 2) - order.begin() <= 1);
      bool rejected = false;
      std::thread wrong([&] { try { runtime.open("Tool/index.html"); } catch (const Error& e) { rejected = e.code == "WRONG_THREAD"; } });
      wrong.join(); CHECK(rejected);
      for (int n = 0; n < 40; ++n) { first->send("CountTracks", {0}); second->send("CountTracks", {0}); }
      until(runtime, [&] { return calls == 81; });
      until(runtime, [&] { return runtime.diagnostics(id)["pendingCalls"] == 0 && runtime.diagnostics(second_id)["pendingCalls"] == 0; });
      CHECK(errors.empty());
      first->bounds["x"] = 123; first->bounds["width"] = 777;
      runtime.tick(); runtime.set_docked(id, true);
      CHECK(runtime.close(id)); runtime.tick(); CHECK(!runtime.is_open(id));
      CHECK(runtime.close(second_id)); runtime.tick();
      auto reopened_id = runtime.open("Tool/index.html");
      auto reopened = windows.back().lock();
      CHECK(reopened->bounds["x"] == 123 && reopened->bounds["width"] == 777 && runtime.is_docked(reopened_id));
      runtime.close(reopened_id); runtime.tick();
    }
    CHECK(docked.empty());
    {
      Runtime runtime(host, root, [&](const std::string& error) { errors.push_back(error); }, docks);
      auto id = runtime.open("Tool/index.html"); auto window = windows.back().lock();
      CHECK(window->bounds["x"] == 123 && window->bounds["width"] == 777 && runtime.is_docked(id));
      result(runtime, *window, window->send("__reawebHello", {1}));
      for (int n = 0; n < 257; ++n) window->send("CountTracks", {0});
      runtime.tick(); CHECK(!runtime.is_open(id));
      CHECK(runtime.diagnostics(id)["stage"] == "failed");
      CHECK(errors.size() == 1 && errors[0].find("queue limit") != std::string::npos);
    }
    CHECK(docked.empty());
    {
      int began = 0, ended = 0;
      host.begin_undo = [&](void* p) { CHECK(p == current); ++began; };
      host.end_undo = [&](void*, const std::string&) { ++ended; };
      host.count_selected_items = [](void*) { return 1; };
      std::string active_take = "take-one";
      host.item_identity = [&](void*, int) { return std::make_pair(std::string("item-one"), active_take); };
      host.event_snapshot = [&](const std::string& event) -> Json {
        return event == "transportchange" ? Json{{"available", true}, {"state", 1}, {"position", 1.5}, {"cursor", 0}, {"tempo", 120}} :
          Json{{"available", true}, {"focused", nullptr}, {"touched", nullptr}};
      };
      Runtime runtime(host, root, [&](const std::string& e) { errors.push_back(e); }, docks);
      auto id = runtime.open("Tool/index.html"); auto window = windows.back().lock();
      result(runtime, *window, window->send("__reawebHello", {1}));
      CHECK(result(runtime, *window, window->send("ReaWeb_WriteFile", {"async.txt", "worker data"}))["result"]["bytes"] == 11);
      CHECK(result(runtime, *window, window->send("ReaWeb_ReadFile", {"async.txt"}))["result"] == "worker data");
      CHECK(result(runtime, *window, window->send("ReaWeb_ClipboardWriteText", {u8"文本"}))["result"] == true);
      CHECK(result(runtime, *window, window->send("ReaWeb_ClipboardReadText"))["result"] == u8"文本");
      CHECK(result(runtime, *window, window->send("ReaWeb_OpenExternal", {"file:///tmp/a"}))["error"]["code"] == "INVALID_URL");
      for (const auto* event : {"itemselectionchange", "takeselectionchange", "transportchange", "fxchange"}) {
        auto initial = result(runtime, *window, window->send("ReaWeb_Subscribe", {event}))["result"];
        if (initial.is_null()) until(runtime, [&] { for (const auto& r : window->responses) if (r.value("event", "") == event) return true; return false; });
      }
      auto before = window->responses.size(); active_take = "take-two"; ++changes;
      until(runtime, [&] { for (size_t i = before; i < window->responses.size(); ++i) if (window->responses[i].value("event", "") == "takeselectionchange") return true; return false; });
      auto token = result(runtime, *window, window->send("ReaWeb_BeginUndo", {"gesture"}))["result"];
      CHECK(token.is_string() && began == 1);
      CHECK(result(runtime, *window, window->send("Main_OnCommand", {40004, 0}))["error"]["code"] == "UNDO_BUSY");
      CHECK(result(runtime, *window, window->send("ReaWeb_EndUndo", Json::array({token})))["result"] == true && ended == 1);
      result(runtime, *window, window->send("ReaWeb_BeginUndo", {"reload"}));
      window->options.on_navigation(); window->document = "second";
      CHECK(ended == 2);
      result(runtime, *window, window->send("__reawebHello", {1}));
      result(runtime, *window, window->send("ReaWeb_BeginUndo", {"close"}));
      runtime.close(id); runtime.tick(); CHECK(ended == 3);
      auto dev = runtime.open_dev("http://127.0.0.1:5173");
      CHECK(windows.back().lock()->options.url == "http://127.0.0.1:5173/");
      runtime.close(dev); runtime.tick();
    }
    fs::remove_all(root);
    std::cout << "Runtime: handshake, worker dispatch, project/reload isolation, subscriptions, keyboard capture, queue limits and persistent docking passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
