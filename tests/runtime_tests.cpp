#include "runtime/runtime.hpp"
#include "host_adapter.hpp"
#include "runtime/services.hpp"
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
  bool shown = true;
  bool drop_enabled = false, drag_result = true;
  bool defer_drag = false;
  Reply drag_reply;
  Json dragged;
  void set_drop_enabled(bool value) override { drop_enabled = value; }
  void start_drag(const Json& payload, Reply reply) override { dragged = payload; if (defer_drag) drag_reply = std::move(reply); else reply({{"result", drag_result}}); }
  int reloads = 0;
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
  bool visible() const override { return shown; }
  void set_visible(bool value) override { shown = value; }
  void reload() override {
    if (options.on_reload && options.on_reload()) return;
    ++reloads; if (options.on_navigation) options.on_navigation();
  }
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
    int selected_count = 1, selected_reads = 0, selection_checks = 0;
    uint64_t selection_signal = 0;
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
    host.count_selected_tracks = [&](void*) { ++selection_checks; return selected_count; };
    host.get_selected_track = [&](void*, int) -> void* { ++selected_reads; return &track; };
    host.event_revision = [&](const std::string& name) { return name == "track-selected" ? selection_signal : uint64_t{0}; };
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
      const auto app = result(runtime, *first, first->send("ReaWeb_GetAppInfo"))["result"];
      CHECK(app["id"] == web["appId"] && app["name"] == "Tool" && app["version"].is_null());
      CHECK(fs::equivalent(fs::u8path(app["rootPath"].get<std::string>()), entry.parent_path()));
      const auto data = fs::u8path(app["dataPath"].get<std::string>());
      CHECK(fs::is_directory(data) && data.filename() == "Data");
      CHECK(data.parent_path().filename().u8string() == app["id"]);
      CHECK(result(runtime, *first, first->send("ReaWeb_WriteFile", {(data / "state.json").u8string(), "{}"}))["result"]["bytes"] == 2);
      CHECK(result(runtime, *first, first->send("ReaWeb_GetPlatform"))["result"] == runtime_platform());
      CHECK(result(runtime, *first, first->send("ReaWeb_GetArchitecture"))["result"] == runtime_architecture());
      CHECK(result(runtime, *first, first->send("ReaWeb_RevealPath", {"missing"}))["error"]["code"] == "FILE_NOT_FOUND");
      CHECK(result(runtime, *first, first->send("ReaWeb_RevealPath", {"index.html"}))["result"] == true);
      const auto dragged = result(runtime, *first, first->send("ReaWeb_DragFiles", Json::array({Json::array({"index.html", "index.html"})})));
      if (dragged.value("result", Json()) != true) throw std::runtime_error("Native drag result: " + dragged.dump());
      CHECK(first->dragged["files"] == Json::array({fs::canonical(entry).u8string()}));
      CHECK(result(runtime, *first, first->send("ReaWeb_DragFiles", Json::array({Json::array()})))["error"]["code"] == "INVALID_ARGUMENT");
      CHECK(result(runtime, *first, first->send("ReaWeb_DragFiles", Json::array({Json::array({"."})})))["error"]["code"] == "INVALID_PATH");
      CHECK(result(runtime, *first, first->send("ReaWeb_DragText", {""}))["error"]["code"] == "INVALID_ARGUMENT");
      first->drag_result = false;
      CHECK(result(runtime, *first, first->send("ReaWeb_DragText", {"text"}))["result"] == false);
      CHECK(first->dragged["text"] == "text");
      result(runtime, *first, first->send("ReaWeb_Subscribe", {"native-drop"}));
      CHECK(first->drop_enabled);
      size_t drops = 0;
      auto count_drops = [&] { size_t count = 0; for (const auto& r : first->responses) if (r.value("event", "") == "native-drop") ++count; return count; };
      CHECK(count_drops() == 0);
      for (int i = 0; i < 3; ++i) first->options.on_drop({{"files", Json::array()}, {"text", std::to_string(i)}, {"x", 0}, {"y", 0}});
      until(runtime, [&] { return count_drops() == 3; });
      for (const auto& r : first->responses) if (r.value("event", "") == "native-drop") CHECK(r["data"]["text"] == std::to_string(drops++));
      result(runtime, *first, first->send("ReaWeb_Unsubscribe", {"native-drop"}));
      CHECK(!first->drop_enabled);
      first->options.on_drop({{"files", Json::array()}, {"text", "ignored"}, {"x", 0}, {"y", 0}});
      result(runtime, *first, first->send("ReaWeb_Subscribe", {"native-drop"}));
      CHECK(count_drops() == 3); // No snapshot replay.
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
      result(runtime, *first, first->send("ReaWeb_Subscribe", {"track-selected"}));
      auto selection_event = [&](size_t start, const char* name, int count) {
        for (size_t i = start; i < first->responses.size(); ++i)
          if (first->responses[i].value("event", "") == name && first->responses[i]["data"]["count"] == count) return true;
        return false;
      };
      // Same-count selection changes need no project edit or 100 ms wait.
      before = first->responses.size(); ++guid[0]; ++selection_signal;
      auto reads = selected_reads;
      runtime.tick(); CHECK(selected_reads > reads);
      until(runtime, [&] { return selection_event(before, "selectionchange", 1) && selection_event(before, "track-selected", 1); });
      // Deselect-all is also observed on the next tick.
      before = first->responses.size(); selected_count = 0; ++selection_signal;
      auto checks = selection_checks;
      runtime.tick(); CHECK(selection_checks > checks);
      until(runtime, [&] { return selection_event(before, "track-selected", 0); });
      // Abandon an unfinished large-selection scan if the selection changes.
      before = first->responses.size(); selected_count = 130; ++selection_signal;
      reads = selected_reads; runtime.tick();
      CHECK(selected_reads > reads && selected_reads - reads <= 64);
      selected_count = 1; ++guid[0]; ++selection_signal;
      reads = selected_reads; runtime.tick(); CHECK(selected_reads > reads);
      until(runtime, [&] { return selection_event(before, "track-selected", 1); });
      CHECK(!selection_event(before, "track-selected", 130));
      // Silent native/API edits still use the fallback poll.
      before = first->responses.size(); ++guid[0];
      until(runtime, [&] { return selection_event(before, "track-selected", 1); });
      // A project edit also invalidates immediately, without a surface callback.
      reads = selected_reads; ++changes; runtime.tick(); CHECK(selected_reads > reads);
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
      result(runtime, *first, first->send("ReaWeb_Open", {"index.html"}));
      CHECK(windows.size() == 2 && platform_count == 1);
      const auto second_id = id + 1; auto second = windows.back().lock();
      result(runtime, *second, second->send("__reawebHello", {1}));
      CHECK(result(runtime, *second, second->send("ReaWeb_GetAppInfo"))["result"] == app);
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
    {
      std::vector<std::string> tracks{"{track-a}"};
      uint64_t markers = 0;
      Json saved{{"available", true}, {"path", "project.rpp"}, {"stamp", "1"}, {"serialization", 0}, {"dirty", false}};
      host.track_count = [&] { CHECK(std::this_thread::get_id() == main_thread); return static_cast<int>(tracks.size()); };
      host.track_identity = [&](int index) { return tracks.at(index); };
      host.event_revision = [&](const std::string&) { return markers; };
      host.project_save_state = [&] { return saved; };
      Runtime runtime(host, root, [&](const std::string&) {}, docks);
      auto id = runtime.open("Tool/index.html"); auto window = windows.back().lock();
      result(runtime, *window, window->send("__reawebHello", {1}));
      runtime.set_docked(id, false);
      CHECK(result(runtime, *window, window->send("ReaWeb_SetBounds", {{{"width", 920}, {"height", 700}}}))["result"]["width"] == 920);
      CHECK(result(runtime, *window, window->send("ReaWeb_SetBounds", {{{"width", -1}}}))["error"]["code"] == "INVALID_ARGUMENT");
      result(runtime, *window, window->send("ReaWeb_SetVisible", {false})); CHECK(!window->shown);
      result(runtime, *window, window->send("ReaWeb_SetVisible", {true})); CHECK(window->shown);
      runtime.set_docked(id, true);
      CHECK(result(runtime, *window, window->send("ReaWeb_SetBounds", {{{"width", 900}}}))["error"]["code"] == "WINDOW_DOCKED");
      runtime.set_docked(id, false);
      CHECK(result(runtime, *window, window->send("ReaWeb_GetTheme"))["result"]["available"] == false);
      result(runtime, *window, window->send("ReaWeb_Log", {{{"level", "info"}, {"message", "runtime test"}}}));
      auto logs = result(runtime, *window, window->send("ReaWeb_GetLogs"))["result"];
      CHECK(logs.back()["message"] == "runtime test" && logs.back()["windowId"] == id);
      auto saw = [&](const std::string& event, size_t after) {
        for (size_t i = after; i < window->responses.size(); ++i) if (window->responses[i].value("event", "") == event) return true;
        return false;
      };
      for (const auto* event : {"track-added", "track-deleted", "marker-changed", "item-changed", "take-changed", "project-loaded", "project-saved", "playback-state-changed", "tempo-changed"})
        result(runtime, *window, window->send("ReaWeb_Subscribe", {event}));
      until(runtime, [&] { return saw("marker-changed", 0); });
      auto before = window->responses.size(); tracks.push_back("{track-b}"); ++markers; ++changes;
      until(runtime, [&] { return saw("track-added", before) && saw("item-changed", before) && saw("take-changed", before); });
      before = window->responses.size(); tracks.erase(tracks.begin());
      until(runtime, [&] { return saw("track-deleted", before); });
      before = window->responses.size(); saved["serialization"] = 1; saved["dirty"] = true;
      const auto pause = Clock::now() + std::chrono::milliseconds(150);
      until(runtime, [&] { return Clock::now() >= pause; }); CHECK(!saw("project-saved", before));
      saved["stamp"] = "2"; saved["dirty"] = false;
      until(runtime, [&] { return saw("project-saved", before); });
      before = window->responses.size(); saved["path"] = "save-as.rpp"; saved["serialization"] = 2;
      until(runtime, [&] { return saw("project-saved", before); });
      before = window->responses.size(); ++generation;
      until(runtime, [&] { return saw("project-loaded", before); });
      result(runtime, *window, window->send("ReaWeb_LifecycleSubscribe", {true}));
      CHECK(window->options.on_reload());
      auto token = window->responses.back()["lifecycle"]["token"];
      result(runtime, *window, window->send("ReaWeb_WriteFile", {"cleanup.json", "saved", {{"overwrite", true}}}));
      CHECK(result(runtime, *window, window->send("ReaWeb_LifecycleComplete", {"wrong"}))["error"]["code"] == "LIFECYCLE_STALE");
      window->send("ReaWeb_LifecycleComplete", Json::array({token}));
      until(runtime, [&] { return window->reloads == 1; });
      CHECK(!runtime.is_ready(id)); window->document = "after-cleanup";
      result(runtime, *window, window->send("__reawebHello", {1}));
      result(runtime, *window, window->send("ReaWeb_LifecycleSubscribe", {true}));
      window->options.on_close(); CHECK(runtime.is_open(id));
      token = window->responses.back()["lifecycle"]["token"];
      window->send("ReaWeb_LifecycleComplete", Json::array({token}));
      until(runtime, [&] { return !runtime.is_open(id); });
      id = runtime.open("Tool/index.html"); window = windows.back().lock();
      result(runtime, *window, window->send("__reawebHello", {1}));
      result(runtime, *window, window->send("ReaWeb_LifecycleSubscribe", {true}));
      runtime.close(id);
      until(runtime, [&] { return !runtime.is_open(id); });
      CHECK(runtime.diagnostics(id)["recentLogs"].back()["message"] == "Lifecycle cleanup timed out after 2000 ms");
    }
    {
      Host isolated; isolated.current_project = [&]() -> void* { return &storage; };
      Runtime runtime(isolated, root, [](const std::string&) {});
      auto open = [&](const std::string& path) {
        const auto id = runtime.open(path); auto window = windows.back().lock();
        result(runtime, *window, window->send("__reawebHello", {1}));
        return std::make_pair(id, window);
      };
      fs::create_directories(root / "Scripts" / "Other"); std::ofstream(root / "Scripts" / "Other" / "index.html") << "<html></html>";
      auto [id, a] = open("Tool/index.html"); auto [other_id, b] = open("Other/index.html");
      const auto info_a = result(runtime, *a, a->send("ReaWeb_GetAppInfo"))["result"];
      const auto info_b = result(runtime, *b, b->send("ReaWeb_GetAppInfo"))["result"];
      CHECK(info_a["id"] != info_b["id"] && info_a["dataPath"] != info_b["dataPath"]);
      a->defer_drag = b->defer_drag = true;
      a->send("ReaWeb_DragText", {"first"}); until(runtime, [&] { return !!a->drag_reply; });
      CHECK(result(runtime, *b, b->send("ReaWeb_DragText", {"busy"}))["error"]["code"] == "DRAG_BUSY");
      runtime.close(id); runtime.tick();
      b->send("ReaWeb_DragText", {"second"}); until(runtime, [&] { return !!b->drag_reply; });
      a->drag_reply({{"result", false}}); // A late completion must not unlock the new source.
      auto [reopened_id, reopened] = open("Tool/index.html");
      CHECK(result(runtime, *reopened, reopened->send("ReaWeb_GetAppInfo"))["result"] == info_a);
      CHECK(result(runtime, *reopened, reopened->send("ReaWeb_DragText", {"still busy"}))["error"]["code"] == "DRAG_BUSY");
      b->drag_reply({{"result", false}});
      CHECK(result(runtime, *reopened, reopened->send("ReaWeb_DragText", {"allowed"}))["result"] == true);
    }
    const auto metadata_root = root / "MetadataApp";
    fs::create_directories(metadata_root);
    std::ofstream(metadata_root / "app.json") << R"({"name":"SendFlow","version":"1.2.3-beta.1"})";
    const auto metadata = app_info(metadata_root, root / "MetadataData", "stable-id");
    CHECK(metadata["name"] == "SendFlow" && metadata["version"] == "1.2.3-beta.1" && metadata["id"] == "stable-id");
    for (const auto& content : {"not json", "[]", R"({"name":4})", R"({"name":"   "})", R"({"version":"bad"})"}) {
      std::ofstream(metadata_root / "app.json") << content;
      bool rejected = false;
      try { app_info(metadata_root, root / "MetadataData", "stable-id"); }
      catch (const Error& error) { rejected = error.code == "APP_MANIFEST_INVALID"; }
      CHECK(rejected);
    }
    fs::remove_all(root);
    std::cout << "Runtime: handshake, worker dispatch, project/reload isolation, subscriptions, keyboard capture, queue limits and persistent docking passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
