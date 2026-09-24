#include "runtime/runtime.hpp"
#include "host_adapter.hpp"
#include "runtime/services.hpp"
#include "platform/shared/devtools.hpp"
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
  std::vector<int> sizes{16, 32};
  std::vector<IconBitmap> icons;
  bool reject_icon = false;
  bool icon_visible = true;
  int icon_target_generation = 0;
  int icon_changes = 0;
  void* icon_target() const override { return reinterpret_cast<void*>(uintptr_t(icon_target_generation + 1)); }
  void set_icon_visible(bool value) override { icon_visible = value; }
  void prepare_dock() override { icons.clear(); icon_visible = true; }
  void restore_floating() override { icons.clear(); icon_visible = true; }
  std::vector<int> icon_sizes() const override { return sizes; }
  void set_icon(const std::vector<IconBitmap>& value) override {
    if (reject_icon) throw Error("ICON_APPLY_FAILED", "Test native failure");
    icons = value; ++icon_changes;
  }
  void clear_icon() override { icons.clear(); ++icon_changes; }
  DevToolsPreferences inspector;
  Json devtools_state() const override { return inspector.state(); }
  void restore_devtools(const Json& value) override { inspector.restore(value); }
  void set_drop_enabled(bool value) override { drop_enabled = value; }
  void start_drag(const Json& payload, Reply reply) override { dragged = payload; if (defer_drag) drag_reply = std::move(reply); else reply({{"result", drag_result}}); }
  int reloads = 0;
  int focuses = 0;
  void focus() override { ++focuses; shown = true; }
  std::vector<Json> responses;
  int sequence = 0;
  std::string document = "first";
  void set_title(const std::string& title) override { options.title = title; }
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
  int send(const std::string& method, Json args = Json::array(), uint64_t epoch = 0, int64_t expires = 0, size_t padding = 0) {
    waiting_on = method + " " + args.dump();
    Json request{{"id", ++sequence}, {"document", document}, {"method", method}, {"args", args}};
    if (epoch) request["project"] = epoch;
    if (expires) request["expiresAt"] = expires;
    options.on_message(request.dump() + std::string(padding, ' '));
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
      CHECK(result(runtime, *first, first->send("ReaWeb_SetIconVisible", {false}))["result"] == true);
      CHECK(!first->icon_visible && first->icons.empty() && !runtime.is_docked(id));
      CHECK(runtime.diagnostics(id)["window"]["iconVisible"] == false);
      CHECK(result(runtime, *first, first->send("ReaWeb_SetIconVisible", {true}))["result"] == true);
      CHECK(first->icon_visible && first->icons.empty());
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
      CHECK(first->options.on_dock_toggle);
      first->options.on_dock_toggle(); runtime.tick(); CHECK(runtime.is_docked(id));
      first->options.on_dock_toggle(); runtime.tick(); CHECK(!runtime.is_docked(id));
      first->options.on_dock_toggle(); first->options.on_dock_toggle(); runtime.tick();
      CHECK(!runtime.is_docked(id) && first->reloads == 0 && runtime.is_ready(id));
      const auto icon_path = entry.parent_path() / "icon.svg";
      std::ofstream(icon_path) << "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'><rect width='16' height='16' fill='red'/></svg>";
      const std::string blue_svg = "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'><rect width='16' height='16' fill='blue'/></svg>";
      const Json page_icon{{"format", ".svg"}, {"bytes", encode_binary(blue_svg.data(), blue_svg.size())}};
      const auto favicon = [&](uint64_t revision, const Json& icon) {
        return first->send("ReaWeb_Favicon", {{{"revision", revision}, {"icon", icon}}});
      };
      const auto begin_icon = [&](uint64_t revision) {
        return result(runtime, *first, first->send("ReaWeb_Favicon", {{{"revision", revision}}}));
      };
      CHECK(begin_icon(1)["result"] == true);
      CHECK(result(runtime, *first, favicon(1, page_icon))["result"] == true);
      CHECK(first->icons[0].rgba[2] == 255 && first->icons[0].rgba[0] == 0);
      CHECK(runtime.diagnostics(id)["window"]["iconVisible"] == true);
      CHECK(result(runtime, *first, first->send("ReaWeb_SetIconVisible", {"false"}))["error"]["code"] == "INVALID_ARGUMENT");
      CHECK(result(runtime, *first, first->send("ReaWeb_SetIconVisible", {false}))["result"] == true);
      CHECK(!first->icon_visible && first->icons[0].rgba[2] == 255);
      CHECK(runtime.set_docked(id, true) && !first->icon_visible && first->icons[0].rgba[2] == 255);
      CHECK(!runtime.set_docked(id, false) && !first->icon_visible && first->icons[0].rgba[2] == 255);
      first->icons.clear(); first->icon_visible = true; ++first->icon_target_generation;
      runtime.tick(); CHECK(!first->icon_visible && first->icons[0].rgba[2] == 255);
      first->icons.clear(); first->icon_visible = true;
      first->options.on_navigation(); first->document = "favicon-reloaded";
      result(runtime, *first, first->send("__reawebHello", {1}));
      CHECK(!first->icon_visible && first->icons[0].rgba[2] == 255);
      CHECK(begin_icon(1)["result"] == true);
      CHECK(result(runtime, *first, favicon(1, page_icon))["result"] == true);
      CHECK(begin_icon(2)["result"] == true);
      CHECK(result(runtime, *first, favicon(1, nullptr))["result"] == false);
      CHECK(result(runtime, *first, favicon(2, {{"format", ".svg"}, {"bytes", encode_binary("invalid", 7)}}))["error"]["code"] == "ICON_INVALID");
      CHECK(first->icons[0].rgba[2] == 255);
      CHECK(begin_icon(3)["result"] == true);
      const auto stale_icon = favicon(3, page_icon);
      CHECK(begin_icon(4)["result"] == true);
      CHECK(result(runtime, *first, favicon(4, nullptr))["result"] == true);
      result(runtime, *first, stale_icon);
      CHECK(first->icons.empty());
      CHECK(begin_icon(5)["result"] == true);
      const auto overridden_icon = favicon(5, page_icon);
      CHECK(result(runtime, *first, first->send("ReaWeb_SetIcon", {"icon.svg"}))["result"] == true);
      result(runtime, *first, overridden_icon);
      CHECK(begin_icon(6)["result"] == false);
      CHECK(result(runtime, *first, favicon(5, nullptr))["result"] == false);
      CHECK(first->icons.size() == 2 && first->icons[0].size == 16 && first->icons[1].size == 32);
      CHECK(first->icons[0].rgba[0] == 255 && first->icons[0].rgba[3] == 255);
      CHECK(!first->icon_visible);
      CHECK(result(runtime, *first, first->send("ReaWeb_SetIconVisible", {true}))["result"] == true);
      CHECK(first->icon_visible && first->icons[0].rgba[0] == 255);
      auto icon_changes = first->icon_changes;
      CHECK(result(runtime, *first, first->send("ReaWeb_SetIcon", {"index.html"}))["error"]["code"] == "ICON_FORMAT");
      CHECK(result(runtime, *first, first->send("ReaWeb_SetIcon", {"missing.png"}))["error"]["code"] == "FILE_NOT_FOUND");
      CHECK(result(runtime, *first, first->send("ReaWeb_SetIcon", {false}))["error"]["code"] == "INVALID_ARGUMENT");
      first->reject_icon = true;
      CHECK(result(runtime, *first, first->send("ReaWeb_SetIcon", {"icon.svg"}))["error"]["code"] == "ICON_APPLY_FAILED");
      first->reject_icon = false;
      CHECK(first->icon_changes == icon_changes);
      fs::remove(icon_path);
      first->sizes = {24, 48};
      until(runtime, [&] { return first->icon_changes > icon_changes; });
      CHECK(first->icons[0].size == 24 && first->icons[1].size == 48 && first->icons[1].rgba[0] == 255);
      for (int cycle = 0; cycle < 3; ++cycle) {
        CHECK(runtime.set_docked(id, true) && first->icons[0].rgba[0] == 255);
        CHECK(!runtime.set_docked(id, false) && first->icons[0].rgba[0] == 255);
      }
      CHECK(result(runtime, *first, first->send("ReaWeb_SetIconVisible", {false}))["result"] == true);
      first->icons.clear(); first->icon_visible = true; ++first->icon_target_generation;
      runtime.tick(); CHECK(!first->icon_visible && first->icons[0].rgba[0] == 255);
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
      first->icons.clear(); first->icon_visible = true;
      first->options.on_navigation(); first->document = "reloaded";
      result(runtime, *first, first->send("__reawebHello", {1}));
      CHECK(calls == 1 && first->response(abandoned).is_null());
      CHECK(begin_icon(1)["result"] == false);
      CHECK(result(runtime, *first, favicon(1, page_icon))["result"] == false);
      CHECK(first->icons[0].rgba[0] == 255 && first->icons[0].rgba[2] == 0 && !first->icon_visible);
      CHECK(runtime.diagnostics(id)["window"]["iconVisible"] == false);
      CHECK(result(runtime, *first, first->send("ReaWeb_SetIconVisible", {true}))["result"] == true);
      CHECK(first->icon_visible && first->icons[0].rgba[0] == 255);
      CHECK(runtime.captures_keyboard(first.get(), [](void*, void*) { return false; }));
      result(runtime, *first, first->send("ReaWeb_Open", {"index.html"}));
      CHECK(windows.size() == 2 && platform_count == 1);
      const auto second_id = id + 1; auto second = windows.back().lock();
      CHECK(second->icon_visible && second->icons.empty());
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
      first->inspector.restore({{"mode", "floating"}, {"widthRatio", 0.63}});
      runtime.tick(); runtime.set_docked(id, true);
      CHECK(runtime.close(id)); runtime.tick(); CHECK(!runtime.is_open(id));
      CHECK(runtime.close(second_id)); runtime.tick();
      auto reopened_id = runtime.open("Tool/index.html");
      auto reopened = windows.back().lock();
      CHECK(reopened->bounds["x"] == 123 && reopened->bounds["width"] == 777 && runtime.is_docked(reopened_id));
      CHECK(reopened->inspector.floating && reopened->inspector.width_ratio == 0.63);
      reopened->inspector.restore({{"mode", "embedded"}, {"widthRatio", 0.57}});
      runtime.close(reopened_id); runtime.tick();
    }
    CHECK(docked.empty());
    {
      Runtime runtime(host, root, [&](const std::string& error) { errors.push_back(error); }, docks);
      auto id = runtime.open("Tool/index.html"); auto window = windows.back().lock();
      CHECK(window->bounds["x"] == 123 && window->bounds["width"] == 777 && runtime.is_docked(id));
      CHECK(!window->inspector.floating && window->inspector.width_ratio == 0.57);
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
    {
      adapter::Host latency_host;
      int latency_calls = 0, selection_count = 1;
      uint64_t revision = 0;
      latency_host.current_project = [&]() -> void* { CHECK(std::this_thread::get_id() == main_thread); return &storage; };
      latency_host.count_tracks = [&](void*) { CHECK(std::this_thread::get_id() == main_thread); ++latency_calls; return 3; };
      latency_host.count_selected_tracks = [&](void*) { return selection_count; };
      latency_host.get_selected_track = [&](void*, int) -> void* { return &track; };
      latency_host.valid_track = [&](void*, void* value) { return value == &track; };
      latency_host.track_guid = [&](void*) { return guid; };
      latency_host.event_revision = [&](const std::string&) { return revision; };
      adapter::connect(latency_host);
      Runtime runtime(latency_host, root, [](const std::string&) {}, docks);
      const auto id = runtime.open("Tool/index.html"); auto window = windows.back().lock();
      result(runtime, *window, window->send("__reawebHello", {1}));
      const auto before = latency_calls;
      const auto request = window->send("CountTracks", {0});
      CHECK(latency_calls == before && window->response(request).is_null()); // No native execution in the WebView callback.
      runtime.tick();
      CHECK(latency_calls == before + 1 && window->response(request)["result"] == 3);
      CHECK(runtime.diagnostics(id)["pendingCalls"] == 0);
      result(runtime, *window, window->send("ReaWeb_Subscribe", {"selectionchange"}));
      until(runtime, [&] { return runtime.diagnostics(id)["pendingCalls"] == 0; });
      const auto response_count = window->responses.size();
      selection_count = 0; ++revision;
      runtime.tick();
      bool delivered = false;
      for (size_t i = response_count; i < window->responses.size(); ++i)
        if (window->responses[i].value("event", "") == "selectionchange" && window->responses[i]["data"]["count"] == 0) delivered = true;
      CHECK(delivered); // Native selection observation and its small notification share the tick.
      // A small request may not jump ahead of an earlier large worker parse.
      const auto large = window->send("ReaWeb_SetTitle", {"first"}, 0, 0, 8192);
      const auto small = window->send("ReaWeb_SetTitle", {"second"});
      CHECK(result(runtime, *window, small)["result"] == true);
      CHECK(window->response(large)["result"] == true);
      CHECK(runtime.diagnostics(id)["window"]["title"] == "second");
      // An already queued fast request is invalidated by navigation as usual.
      const auto stale = window->send("CountTracks", {0});
      window->options.on_navigation();
      runtime.tick();
      CHECK(window->response(stale).is_null() && latency_calls == before + 1);
    }
    {
      Runtime runtime(host, root, [](const std::string&) {}, docks);
      auto id = runtime.open("Tool/index.html"); auto a = windows.back().lock();
      auto other_id = runtime.open("Tool/index.html"); auto b = windows.back().lock();
      auto rejects = [](const std::function<void()>& call, const std::string& code) {
        try { call(); } catch (const Error& e) { CHECK(e.code == code); return; }
        throw std::runtime_error("Expected " + code);
      };
      CHECK(runtime.receive(id).empty());
      rejects([&] { runtime.send(id, "early"); }, "NOT_READY");
      for (auto w : {a, b}) result(runtime, *w, w->send("__reawebHello", {1}));
      auto messages = [](const FakeWindow& w) {
        std::vector<std::string> values;
        for (const auto& r : w.responses) if (r.value("event", "") == "message") values.push_back(r.at("data").get<std::string>());
        return values;
      };
      const std::vector<std::string> sent = {"A", std::string(8192, 'x'), u8"鼓组 🎛️\n\"\\", "{\"json\":true}", "", "C"};
      for (const auto& text : sent) CHECK(runtime.send(id, text));
      runtime.tick(); CHECK(messages(*a).empty());
      result(runtime, *a, a->send("ReaWeb_Subscribe", {"message"}));
      until(runtime, [&] { return messages(*a).size() == sent.size(); });
      CHECK(messages(*a) == sent && messages(*b).empty());
      std::vector<int> requests;
      for (const auto& text : sent) requests.push_back(a->send("ReaWeb_HostSend", {text}));
      for (auto request : requests) CHECK(result(runtime, *a, request)["result"] == true);
      CHECK(runtime.receive(other_id).empty());
      for (const auto& text : sent) CHECK(runtime.receive(id) == text);
      CHECK(runtime.receive(id).empty());
      CHECK(result(runtime, *b, b->send("ReaWeb_HostSend", {"only b"}))["result"] == true);
      CHECK(runtime.receive(id).empty() && runtime.receive(other_id) == "only b");
      rejects([&] { runtime.send(id, std::string(host_message_limit + 1, 'x')); }, "MESSAGE_LIMIT");
      rejects([&] { runtime.send(id, std::string("bad\0text", 8)); }, "INVALID_ARGUMENT");
      rejects([&] { runtime.send(id, std::string("\xff")); }, "INVALID_ARGUMENT");
      CHECK(result(runtime, *a, a->send("ReaWeb_HostSend", {std::string(host_message_limit + 1, 'x')}))["error"]["code"] == "MESSAGE_LIMIT");
      CHECK(result(runtime, *a, a->send("ReaWeb_HostSend", {std::string("a\0b", 3)}))["error"]["code"] == "INVALID_ARGUMENT");
      CHECK(result(runtime, *a, a->send("ReaWeb_HostSend", {42}))["error"]["code"] == "INVALID_ARGUMENT");
      result(runtime, *a, a->send("ReaWeb_Unsubscribe", {"message"}));
      for (size_t i = 0; i < host_queue_limit; ++i) CHECK(runtime.send(id, std::to_string(i)));
      rejects([&] { runtime.send(id, "overflow"); }, "QUEUE_LIMIT");
      for (size_t i = 0; i < host_queue_limit; ++i)
        CHECK(result(runtime, *a, a->send("ReaWeb_HostSend", {std::to_string(i)}))["result"] == true);
      CHECK(result(runtime, *a, a->send("ReaWeb_HostSend", {"overflow"}))["error"]["code"] == "QUEUE_LIMIT");
      for (size_t i = 0; i < host_queue_limit; ++i) CHECK(runtime.receive(id) == std::to_string(i));
      const auto before = messages(*a).size();
      result(runtime, *a, a->send("ReaWeb_Subscribe", {"message"}));
      until(runtime, [&] { return messages(*a).size() == before + host_queue_limit; });
      const auto all = messages(*a);
      for (size_t i = 0; i < host_queue_limit; ++i) CHECK(all[before + i] == std::to_string(i));
      result(runtime, *a, a->send("ReaWeb_Unsubscribe", {"message"}));
      const std::string block(host_message_limit, 'x');
      for (int i = 0; i < 8; ++i) CHECK(runtime.send(id, block));
      for (int i = 0; i < 8; ++i) CHECK(result(runtime, *a, a->send("ReaWeb_HostSend", {block}))["result"] == true);
      rejects([&] { runtime.send(id, "x"); }, "QUEUE_LIMIT");
      CHECK(result(runtime, *a, a->send("ReaWeb_HostSend", {"x"}))["error"]["code"] == "QUEUE_LIMIT");
      CHECK(runtime.receive(id) == block);
      CHECK(runtime.send(id, "space reclaimed"));
      // Navigation cancels both retained messages and requests still on the worker.
      a->send("ReaWeb_HostSend", {"stale request"}, 0, 0, 8192);
      a->options.on_navigation(); a->document = "second";
      CHECK(runtime.receive(id).empty());
      result(runtime, *a, a->send("__reawebHello", {1}));
      const auto after = messages(*a).size();
      result(runtime, *a, a->send("ReaWeb_Subscribe", {"message"}));
      a->document = "first";
      CHECK(result(runtime, *a, a->send("ReaWeb_HostSend", {"old token"}))["error"]["code"] == "DOCUMENT_STALE");
      a->document = "second";
      CHECK(runtime.receive(id).empty());
      runtime.send(id, "fresh");
      until(runtime, [&] { return messages(*a).size() > after; });
      CHECK(messages(*a).size() == after + 1 && messages(*a).back() == "fresh");
      bool wrong_send = false, wrong_receive = false;
      std::thread wrong([&] {
        try { runtime.send(id, "thread"); } catch (const Error& e) { wrong_send = e.code == "WRONG_THREAD"; }
        try { runtime.receive(id); } catch (const Error& e) { wrong_receive = e.code == "WRONG_THREAD"; }
      }); wrong.join(); CHECK(wrong_send && wrong_receive);
      runtime.send(id, block); runtime.tick();
      const auto closed_count = messages(*a).size();
      CHECK(runtime.close(id));
      rejects([&] { runtime.send(id, "closed"); }, "WINDOW_CLOSED");
      rejects([&] { runtime.receive(id); }, "WINDOW_CLOSED");
      until(runtime, [&] { return runtime.diagnostics(id)["stage"] == "closed"; });
      CHECK(messages(*a).size() == closed_count);
      auto next_id = runtime.open("Tool/index.html");
      CHECK(runtime.receive(next_id).empty());
      CHECK(runtime.is_open(other_id));
    }
    {
      Runtime runtime(host, root, [](const std::string&) {}, docks);
      const auto id = runtime.open("Tool/index.html");
      auto window = windows.back().lock();
      result(runtime, *window, window->send("__reawebHello", {1}));
      runtime.set_docked(id, true);
      runtime.send(id, "pending Lua message");
      CHECK(result(runtime, *window, window->send("ReaWeb_HostSend", {"pending UI message"}))["result"] == true);
      auto close = window->options.on_close;
      std::weak_ptr<Window> released = window;
      window.reset();
      close(); close();
      CHECK(!runtime.is_open(id));
      until(runtime, [&] { return released.expired(); });
      CHECK(runtime.diagnostics(id)["stage"] == "closed");
      const auto reopened = runtime.open("Tool/index.html");
      CHECK(reopened != id && runtime.receive(reopened).empty());
    }
    {
      Runtime runtime(host, root, [&](const std::string& error) { errors.push_back(error); }, docks);
      const auto script = entry.parent_path() / "Open.lua";
      std::ofstream(script) << "-- launcher";
      const auto id = runtime.open_instance("Tool/index.html", script.u8string());
      auto first = windows.back().lock();
      CHECK(runtime.open_instance("Tool/missing.html", script.u8string()) == id);
      CHECK(first->focuses == 1 && !runtime.is_ready(id));
      CHECK(runtime.open_instance("Tool/index.html", "@" + script.u8string()) != id);
      const auto exact = runtime.open_instance("Tool/index.html", "opaque:Ä:key");
      CHECK(runtime.open_instance("Tool/index.html", "opaque:Ä:key") == exact);
      CHECK(runtime.open_instance("Tool/index.html", "opaque:ä:key") != exact);
      CHECK(runtime.open_instance("Tool/index.html", "") != runtime.open_instance("Tool/index.html", ""));
      CHECK(runtime.open_instance("Tool/index.html", "", "Settings") != runtime.open_instance("Tool/index.html", "", "Settings"));
      CHECK(runtime.open_instance("Tool/index.html", "a:b", "c") != runtime.open_instance("Tool/index.html", "a", "b:c"));
      CHECK(runtime.open_instance("Tool/index.html", (script.parent_path() / "Other.lua").u8string()) != id);
      auto copy = root / "Scripts" / "Copy";
      fs::create_directories(copy); std::ofstream(copy / "index.html") << "<html></html>";
      CHECK(runtime.open_instance("Copy/index.html", (copy / "Open.lua").u8string()) != id);
      const auto settings = runtime.open_instance("Tool/index.html", script.u8string(), "Settings");
      CHECK(settings != id && runtime.open_instance("Tool/index.html", script.u8string(), "Settings") == settings);
      CHECK(runtime.open_instance("Tool/index.html", script.u8string(), "settings") != settings);
      const auto many = runtime.open_instance("Tool/index.html", script.u8string(), "", true);
      CHECK(many != id && runtime.open_instance("Tool/index.html", script.u8string(), "", true) != many);
      CHECK(runtime.open_instance("Tool/index.html", script.u8string()) == id);
      CHECK(runtime.open("Tool/index.html") != runtime.open("Tool/index.html"));
      runtime.set_docked(id, true);
      first->shown = false;
      CHECK(runtime.open_instance("Tool/index.html", script.u8string()) == id && first->shown);
      CHECK(runtime.is_docked(id));
      first->is_closed = true;
      const auto replacement = runtime.open_instance("Tool/index.html", script.u8string());
      CHECK(replacement != id);
      runtime.tick();
      CHECK(runtime.open_instance("Tool/index.html", script.u8string()) == replacement);
      runtime.close(replacement);
      const auto reopened = runtime.open_instance("Tool/index.html", script.u8string());
      CHECK(reopened != replacement);
      windows.back().lock()->options.on_error("Test initialization failure");
      CHECK(runtime.open_instance("Tool/index.html", script.u8string()) != reopened);
    }
    {
      int refreshed = 0;
      auto title_docks = docks;
      title_docks.refresh = [&](void*) { ++refreshed; };
      Runtime runtime(host, root, [&](const std::string& error) { errors.push_back(error); }, title_docks);
      const auto id = runtime.open_instance("Tool/index.html", "@private/launcher.lua");
      auto window = windows.back().lock();
      const auto fallback = window->options.title;
      CHECK(fallback == "ReaWebAPI — Tool");
      result(runtime, *window, window->send("__reawebHello", {1}));
      const auto title = [&](const std::string& text) {
        return result(runtime, *window, window->send("ReaWeb_DocumentTitle", {text}));
      };
      CHECK(title("")["result"] == true && window->options.title == fallback);
      CHECK(title("SendFlow")["result"] == true && window->options.title == "SendFlow");
      CHECK(runtime.open_instance("missing.html", "@private/launcher.lua") == id);
      CHECK(window->options.title == "SendFlow");
      CHECK(title("我的工具 🎵")["result"] == true);
      CHECK(runtime.diagnostics(id)["window"]["title"] == "我的工具 🎵");
      CHECK(title(std::string(255, 'a') + "🎵")["result"] == true);
      CHECK(window->options.title == std::string(255, 'a'));
      CHECK(title(std::string(252, 'a') + "🎵x")["result"] == true);
      CHECK(window->options.title == std::string(252, 'a') + "🎵");
      CHECK(title(std::string("a\0b", 3))["error"]["code"] == "INVALID_ARGUMENT");
      for (const auto& args : {Json::array(), Json::array({12}), Json::array({"a", "b"})})
        CHECK(result(runtime, *window, window->send("ReaWeb_DocumentTitle", args))["error"]["code"] == "INVALID_ARGUMENT");
      CHECK(title("")["result"] == true && window->options.title == fallback);
      runtime.set_docked(id, true);
      const auto prior_refresh = refreshed;
      CHECK(title("Docked title")["result"] == true && refreshed == prior_refresh + 1);
      CHECK(window->options.title == "Docked title");
      const auto abandoned = window->send("ReaWeb_DocumentTitle", {"Old document"});
      window->options.on_navigation(); window->document = "new-title-document";
      result(runtime, *window, window->send("__reawebHello", {1}));
      CHECK(window->response(abandoned).is_null() && window->options.title == fallback);
      CHECK(title("ReaGBA")["result"] == true && window->options.title == "ReaGBA");
      for (const auto& invalid : {std::string(), std::string(257, 'x'), std::string("a\0b", 3)})
        CHECK(result(runtime, *window, window->send("ReaWeb_SetTitle", {invalid}))["error"]["code"] == "INVALID_ARGUMENT");
      CHECK(title("Still automatic")["result"] == true);
      CHECK(result(runtime, *window, window->send("ReaWeb_SetTitle", {"Explicit"}))["result"] == true);
      CHECK(title("Ignored")["result"] == false && window->options.title == "Explicit");
      window->options.on_navigation(); window->document = "explicit-reload";
      result(runtime, *window, window->send("__reawebHello", {1}));
      CHECK(title("Reloaded HTML")["result"] == false && window->options.title == "Explicit");
      CHECK(runtime.open_instance("Tool/index.html", "@private/launcher.lua") == id);
      runtime.set_docked(id, false);
      CHECK(window->options.title == "Explicit");
      const auto other = runtime.open("Tool/index.html");
      auto second = windows.back().lock();
      result(runtime, *second, second->send("__reawebHello", {1}));
      CHECK(result(runtime, *second, second->send("ReaWeb_DocumentTitle", {"Independent"}))["result"] == true);
      CHECK(second->options.title == "Independent" && window->options.title == "Explicit");
      CHECK(runtime.diagnostics(other)["window"]["title"] == "Independent");
    }
    const auto metadata_root = root / "MetadataApp";
    DevToolsPreferences inspector;
    CHECK(!inspector.floating && inspector.width_ratio == 0.4);
    for (const auto& invalid : {Json(), Json::array(), Json{{"mode", 12}, {"widthRatio", "wide"}}, Json{{"mode", "invalid"}, {"widthRatio", nullptr}}}) inspector.restore(invalid);
    CHECK(!inspector.floating && inspector.width_ratio == 0.4);
    inspector.restore({{"mode", "floating"}, {"widthRatio", 5}});
    CHECK(inspector.floating && inspector.width_ratio == 0.8);
    inspector.restore({{"mode", "embedded"}, {"widthRatio", -5}});
    CHECK(!inspector.floating && inspector.width_ratio == 0.2);
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
