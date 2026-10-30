#include "runtime/runtime.hpp"
#include "host_adapter.hpp"
#include "runtime/services.hpp"
#include "platform/shared/devtools.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <set>
using namespace reaweb;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("Check failed: " #condition); } while (false)
namespace {
int platform_count = 0;
int platform_pumps = 0;
int platform_idles = 0;
unsigned platform_capacity = 0;
std::vector<fs::path> profiles;
std::string waiting_on;
bool defer_next_presentation = false;
bool fail_next_presentation = false;
void* jsfx_floating = nullptr;
double jsfx_values[2] = {.25, .5};
int jsfx_writes = 0, jsfx_ends = 0, jsfx_fail_param = -1;
struct FakeWindow : Window {
  WindowOptions options;
  bool daw_keyboard = false;
  bool is_closed = false;
  bool shown = true;
  bool deferred = false, wants_visible = true, presentation_cancelled = false;
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
  void focus() override { if (!presentation_cancelled) { ++focuses; wants_visible = shown = true; const bool pending = deferred; deferred = false; if (pending && options.on_present) options.on_present(); } }
  void show() override { if (!presentation_cancelled) { deferred = false; wants_visible = shown = true; } }
  std::vector<Json> responses;
  std::chrono::milliseconds message_delay{0}, frame_delay{0}, tick_delay{0};
  void tick() override { std::this_thread::sleep_for(tick_delay); }
  int sequence = 0;
  std::string document = "first";
  void set_title(const std::string& title) override { options.title = title; }
  Json bounds = {{"x", 40}, {"y", 50}, {"width", 860}, {"height", 640}, {"maximized", false}};
  explicit FakeWindow(WindowOptions value) : options(std::move(value)) {
    deferred = defer_next_presentation || options.hidden; defer_next_presentation = false;
    wants_visible = !options.hidden; shown = !deferred;
  }
  void evaluate(const std::string& script) override {
    if (script.rfind("window.__reawebSetDawKeyboard?.(", 0) == 0) {
      daw_keyboard = script.find("(true)") != std::string::npos; return;
    }
    const auto start = script.find('(') + 1;
    auto message = Json::parse(script.substr(start, script.size() - start - 2));
    if (message.value("event", "") == "message") std::this_thread::sleep_for(message_delay);
    if (message.value("event", "") == "frame") std::this_thread::sleep_for(frame_delay);
    if (!message.value("started", false)) responses.push_back(std::move(message));
  }
  void devtools() override {}
  bool closed() const override { return is_closed; }
  void show_after_create() override {
    if (fail_next_presentation) { fail_next_presentation = false; throw Error("TRANSPARENCY_FAILED", "Injected native host setup failure"); }
  }
  bool frameless_active() const override { return options.frameless && !(options.is_docked && options.is_docked()); }
  double radius = 0;
  void set_frameless(bool value) override { options.frameless = value; }
  void set_corner_radius(double value) override { radius = value; }
  bool corner_radius_active() const override { return radius > 0 && frameless_active(); }
  bool transparent_active() const override { return options.transparent && !is_closed; }
  void* native_handle() const override { return const_cast<FakeWindow*>(this); }
  Json placement() const override { return bounds; }
  void restore_placement(const Json& value) override { bounds = value; }
  bool visible() const override { return shown; }
  bool visibility_requested() const override { return deferred ? wants_visible : shown; }
  bool presentation_pending() const override { return deferred; }
  void cancel_pending_show() override { if (deferred) presentation_cancelled = true; }
  std::function<void()> on_hide;
  void set_visible(bool value) override {
    wants_visible = value; shown = value && !deferred;
    if (!value && on_hide) { auto callback = std::move(on_hide); callback(); }
  }
  void reparent_host(void* parent) override { options.parent = parent; deferred = false; set_visible(false); }
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
  void pump() override { ++platform_pumps; }
  void set_idle_capacity(unsigned count) override { platform_capacity = count; }
  void idle() override { ++platform_idles; }
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
    std::ofstream(entry.parent_path() / "app.json") << Json{{"id", "test-" + state_key(entry.parent_path().u8string(), 0)}};
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
      auto keyboard_docks = docks;
      std::string keys;
      keyboard_docks.daw_space = [&](bool down, bool repeat) { keys += down ? (repeat ? 'r' : 'd') : 'u'; };
      Runtime runtime(host, root, [&](const std::string& error) { errors.push_back(error); }, keyboard_docks);
      const auto id = runtime.open(entry.u8string()); auto window = windows.back().lock();
      result(runtime, *window, window->send("__reawebHello", {1}));
      CHECK(runtime.diagnostics(id)["window"]["dawKeyboard"] == false);
      window->options.on_message("__reawebDawKey:d"); runtime.tick(); CHECK(keys.empty());
      CHECK(result(runtime, *window, window->send("ReaWeb_SetDawKeyboard", {"true"}))["error"]["code"] == "INVALID_ARGUMENT");
      CHECK(result(runtime, *window, window->send("ReaWeb_SetDawKeyboard", {true}))["result"] == true);
      CHECK(window->daw_keyboard && runtime.diagnostics(id)["window"]["dawKeyboard"] == true);
      CHECK(result(runtime, *window, window->send("ReaWeb_SetKeyboardCapture", {false}))["result"] == false);
      CHECK(!runtime.captures_keyboard(window.get(), [](void*, void*) { return false; }));
      CHECK(runtime.captures_keyboard(window.get(), [](void*, void*) { return false; }, true));
      for (const auto key : {"u", "r", "d", "d", "r", "u", "u"}) window->options.on_message(std::string("__reawebDawKey:") + key);
      CHECK(keys.empty()); runtime.tick(); CHECK(keys == "dru");
      const auto second_id = runtime.open(entry.u8string()); auto second = windows.back().lock();
      result(runtime, *second, second->send("__reawebHello", {1}));
      CHECK(runtime.diagnostics(second_id)["window"]["dawKeyboard"] == false);
      second->options.on_message("__reawebDawKey:d"); runtime.tick(); CHECK(keys == "dru");
      window->options.on_message("__reawebDawKey:d"); runtime.tick(); CHECK(keys == "drud");
      CHECK(result(runtime, *window, window->send("ReaWeb_SetDawKeyboard", {false}))["result"] == false);
      CHECK(keys == "drudu" && !window->daw_keyboard);
      window->options.on_message("__reawebDawKey:r"); window->options.on_message("__reawebDawKey:u"); runtime.tick(); CHECK(keys == "drudu");
      result(runtime, *window, window->send("ReaWeb_SetDawKeyboard", {true}));
      window->options.on_message("__reawebDawKey:d"); runtime.tick();
      window->reload(); CHECK(keys == "drududu");
      CHECK(runtime.diagnostics(id)["window"]["dawKeyboard"] == false);
      window->document = "ordinary-reloaded";
      result(runtime, *window, window->send("__reawebHello", {1}));
      result(runtime, *window, window->send("ReaWeb_SetDawKeyboard", {true}));
      window->options.on_message("__reawebDawKey:d"); runtime.tick();
      runtime.close(id); runtime.tick(); CHECK(keys == "drudududu");
      runtime.close(second_id); runtime.tick();
    }
    {
      const auto directory = root / "Scripts" / "KeyboardConfig";
      fs::create_directories(directory);
      const auto page = directory / "index.html", manifest = directory / "app.json";
      std::ofstream(page) << "<!doctype html>";
      const Json config{{"id", "keyboard-config"}, {"dawKeyboard", true}};
      std::ofstream(manifest) << config;
      auto keyboard_docks = docks;
      keyboard_docks.daw_space = [](bool, bool) {};
      Runtime runtime(host, root, [](const std::string&) {}, keyboard_docks);
      const auto id = runtime.open(page.u8string()); auto window = windows.back().lock();
      result(runtime, *window, window->send("__reawebHello", {1}));
      CHECK(window->daw_keyboard && runtime.diagnostics(id)["window"]["dawKeyboard"] == true);
      result(runtime, *window, window->send("ReaWeb_SetDawKeyboard", {false}));
      CHECK(!window->daw_keyboard && Json::parse(std::ifstream(manifest)) == config);
      const auto second_id = runtime.open(page.u8string()); auto second = windows.back().lock();
      result(runtime, *second, second->send("__reawebHello", {1}));
      CHECK(second->daw_keyboard && !window->daw_keyboard);
      window->reload(); window->document = "configured-reload";
      result(runtime, *window, window->send("__reawebHello", {1}));
      CHECK(window->daw_keyboard);
      auto changed = config; changed["dawKeyboard"] = false;
      std::ofstream(manifest) << changed;
      window->reload(); window->document = "changed-config";
      result(runtime, *window, window->send("__reawebHello", {1}));
      CHECK(!window->daw_keyboard && second->daw_keyboard);
      changed["dawKeyboard"] = "true"; std::ofstream(manifest) << changed;
      window->reload(); window->document = "invalid-config";
      CHECK(result(runtime, *window, window->send("__reawebHello", {1}))["error"]["code"] == "APP_MANIFEST_INVALID");
      CHECK(!runtime.is_ready(id));
      bool rejected = false;
      try { runtime.open(page.u8string()); } catch (const Error& error) { rejected = error.code == "APP_MANIFEST_INVALID"; }
      CHECK(rejected);
      runtime.close(id); runtime.close(second_id); runtime.tick();
    }
    {
      struct Editor final : JsfxEditorHost {
        bool alive = true;
        int size_requests = 0;
        void* parent() const override { return const_cast<Editor*>(this); }
        bool valid() const override { return alive; }
        std::optional<int> selected_fx() const override { return alive ? 0 : -1; }
        void resize(void*) override {}
        void present(void*) override {}
        void restore() override {}
        int width = 0, height = 0; bool exact = false;
        void request_size(int w, int h, bool e = false) override { ++size_requests; width = w; height = h; exact = e; }
      };
      struct Discovery final : JsfxEditorDiscovery {
        JsfxEditor editor;
        bool found = false;
        std::vector<JsfxEditor> scan(reaweb::Host&, Match) override {
          if (found) return {};
          found = true; return {editor};
        }
      };
      const auto resource = root / "JSFXProfile", directory = resource / "Effects/Test";
      fs::create_directories(directory);
      std::ofstream(directory / "test.jsfx") << "desc:Test\n@sample\n";
      std::ofstream(directory / "test.json") << R"({"schema":1,"JSFX_WebUI":{"entry":"index.html"}})";
      std::ofstream(directory / "app.json") << R"({"id":"jsfx-keyboard-config","dawKeyboard":false})";
      std::ofstream(directory / "index.html") << "<html></html>";
      auto jsfx_host = host;
      int jsfx_undo_begin = 0, jsfx_undo_end = 0;
      jsfx_host.begin_undo = [&](void*) { ++jsfx_undo_begin; };
      jsfx_host.end_undo = [&](void*, const std::string&) { ++jsfx_undo_end; };
      jsfx_host.native_function = [](const char* name) -> void* {
        if (!std::strcmp(name, "TrackFX_GetCount")) return reinterpret_cast<void*>(+[](void*) { return 1; });
        if (!std::strcmp(name, "TrackFX_GetFloatingWindow")) return reinterpret_cast<void*>(+[](void*, int) -> void* { return jsfx_floating; });
        if (!std::strcmp(name, "TrackFX_GetFXGUID")) return reinterpret_cast<void*>(+[](void*, int) -> Guid* { static Guid guid{42}; return &guid; });
        if (!std::strcmp(name, "TrackFX_GetNumParams")) return reinterpret_cast<void*>(+[](void*, int) { return 2; });
        if (!std::strcmp(name, "TrackFX_GetParamEx")) return reinterpret_cast<void*>(+[](void*, int, int p, double* lo, double* hi, double* mid) {
          *lo = 0; *hi = 1; *mid = .5; return jsfx_values[p];
        });
        if (!std::strcmp(name, "TrackFX_GetParamNormalized")) return reinterpret_cast<void*>(+[](void*, int, int p) { return jsfx_values[p]; });
        if (!std::strcmp(name, "TrackFX_GetParamName") || !std::strcmp(name, "TrackFX_GetParamIdent") || !std::strcmp(name, "TrackFX_GetFormattedParamValue"))
          return reinterpret_cast<void*>(+[](void*, int, int, char* out, int) { std::strcpy(out, "value"); return true; });
        if (!std::strcmp(name, "TrackFX_GetParameterStepSizes")) return reinterpret_cast<void*>(+[](void*, int, int, double*, double*, double*, bool*) { return false; });
        if (!std::strcmp(name, "TrackFX_SetParamNormalized")) return reinterpret_cast<void*>(+[](void*, int, int p, double v) {
          if (p == jsfx_fail_param) return false; ++jsfx_writes; jsfx_values[p] = v; return true;
        });
        if (!std::strcmp(name, "TrackFX_EndParamEdit")) return reinterpret_cast<void*>(+[](void*, int, int) { ++jsfx_ends; return true; });
        if (!std::strcmp(name, "TrackFX_GetNamedConfigParm")) return reinterpret_cast<void*>(+[](void*, int, const char* key, char* out, int) {
          const char* value = !std::strcmp(key, "fx_type") ? "JS" : !std::strcmp(key, "fx_ident") ? "Test/test.jsfx" : nullptr;
          if (!value) return false; std::strcpy(out, value); return true;
        });
        return adapter::resolve(name);
      };
      auto editor = std::make_shared<Editor>();
      auto discovery = std::make_shared<Discovery>(); discovery->editor = {&track, 0, false, editor.get(), editor};
      auto keyboard_docks = docks;
      std::string keys;
      keyboard_docks.daw_space = [&](bool down, bool repeat) { keys += down ? (repeat ? 'r' : 'd') : 'u'; };
      Runtime runtime(jsfx_host, resource, [&](const std::string& error) { errors.push_back(error); }, keyboard_docks);
      const auto before = windows.size(); runtime.initialize_jsfx(discovery);
      until(runtime, [&] { return windows.size() > before; });
      auto window = windows.back().lock(); const int id = 1;
      result(runtime, *window, window->send("__reawebHello", {1}));
      CHECK(window->options.script.find("__reawebDawKey:") != std::string::npos);
      CHECK(runtime.captures_keyboard(window.get(), [](void*, void*) { return false; }, true));
      for (const auto key : {"u", "r", "d", "d", "r", "u", "u"}) window->options.on_message(std::string("__reawebDawKey:") + key);
      CHECK(keys.empty());
      runtime.tick(); CHECK(keys == "dru");
      CHECK(result(runtime, *window, window->send("ReaWeb_SetDawKeyboard", {false}))["result"] == false);
      CHECK(!window->daw_keyboard && !runtime.diagnostics(id)["window"]["dawKeyboard"].get<bool>());
      const auto association_path = directory / "test.json";
      const auto original_association = Json::parse(std::ifstream(association_path));
      auto disabled_association = original_association;
      disabled_association["JSFX_WebUI"]["dawKeyboard"] = false;
      std::ofstream(association_path) << disabled_association;
      window->reload(); window->document = "jsfx-disabled-config";
      result(runtime, *window, window->send("__reawebHello", {1}));
      CHECK(!window->daw_keyboard && !runtime.diagnostics(id)["window"]["dawKeyboard"].get<bool>());
      CHECK(result(runtime, *window, window->send("ReaWeb_SetDawKeyboard", {true}))["result"] == true);
      result(runtime, *window, window->send("ReaWeb_SetKeyboardCapture", {false}));
      CHECK(runtime.captures_keyboard(window.get(), [](void*, void*) { return false; }, true));
      CHECK(Json::parse(std::ifstream(association_path)) == disabled_association);
      window->reload(); window->document = "jsfx-config-restored";
      result(runtime, *window, window->send("__reawebHello", {1}));
      CHECK(!window->daw_keyboard);
      std::ofstream(association_path) << original_association;
      window->reload(); window->document = "jsfx-default-config";
      result(runtime, *window, window->send("__reawebHello", {1}));
      CHECK(window->daw_keyboard);
      const auto call = [&](const char* method, Json payload = Json::object()) {
        return result(runtime, *window, window->send("ReaWeb_ServiceInvoke", {"jsfx", method, payload}));
      };
      const auto initial_read = [&](const char* method, Json payload = Json::object()) {
        const auto request = window->send("ReaWeb_ServiceInvoke", {"jsfx", method, payload});
        const auto matches = [request](const Json& response) { return response.value("id", 0) == request; };
        CHECK(std::none_of(window->responses.begin(), window->responses.end(), matches));
        runtime.tick();
        const auto response = std::find_if(window->responses.begin(), window->responses.end(), matches);
        CHECK(response != window->responses.end());
        CHECK(!window->visible());
        return *response;
      };
      const auto capability = initial_read("getCapabilities")["result"];
      CHECK(capability["ok"] == true && capability["data"]["status"] == "parameterOnly");
      const auto session_id = capability["sessionId"];
      CHECK(call("getState", {{"sessionId", session_id}})["result"]["error"]["code"] == "UNKNOWN_METHOD");
      CHECK(initial_read("getParamState", {{"sessionId", "stale"}})["result"]["error"]["code"] == "STALE_SESSION");
      CHECK(call("openStream", {{"sessionId", session_id}, {"id", "spectrum"}})["result"]["error"]["code"] == "UNSUPPORTED");
      CHECK(initial_read("getEditorSize", {{"sessionId", session_id}})["result"]["data"]["customized"] == false);
      const auto first = initial_read("getParamState", {{"sessionId", session_id}})["result"]["data"];
      CHECK(call("present", {{"sessionId", session_id}, {"activation", first["activation"]}})["result"]["data"]["presented"] == true);
      CHECK(window->visible() && runtime.diagnostics(id)["jsfx"]["presented"] == true);
      const Json context{{"sessionId", session_id}, {"activation", first["activation"]}};
      const auto edit = [&](const char* method, Json payload = Json::object()) { payload.update(context); return call(method, payload)["result"]; };
      CHECK(edit("getEditorSize")["data"]["customized"] == false);
      std::ofstream(directory / "test.json") << R"({"schema":1,"JSFX_WebUI":{"entry":"index.html","width":800,"height":400}})";
      CHECK(edit("getEditorSize")["data"]["width"] == 800);
      CHECK(edit("setEditorSize", {{"width", 480}, {"height", 240}})["data"]["customized"] == true);
      std::ofstream(directory / "test.json") << R"({"schema":1,"JSFX_WebUI":{"entry":"index.html","width":1024,"height":768}})";
      CHECK(edit("getEditorSize")["data"]["width"] == 480);
      CHECK(editor->width == 480 && editor->height == 240 && editor->exact);
      CHECK(edit("setEditorSize", {{"width", 99}, {"height", 240}})["error"]["code"] == "INVALID_ARGUMENT");
      CHECK(edit("setEditorSize", {{"width", 480.5}, {"height", 240}})["error"]["code"] == "INVALID_ARGUMENT");
      CHECK(call("setEditorSize", {{"sessionId", session_id}, {"activation", 0}, {"width", 640}, {"height", 420}})["result"]["error"]["code"] == "FX_STALE");
      auto group = edit("beginParamEditGroup", {{"indices", {0, 1}}})["data"]["gestureId"];
      CHECK(edit("beginParamEdit", {{"index", 0}})["error"]["code"] == "GESTURE_BUSY");
      CHECK(edit("setParams", {{"gestureId", group}, {"values", {{{"index", 0}, {"normalized", .1}}, {{"index", 1}, {"normalized", 2}}}}})["error"]["code"] == "INVALID_ARGUMENT");
      CHECK(jsfx_writes == 0);
      CHECK(edit("setParams", {{"gestureId", group}, {"values", {{{"index", 0}, {"normalized", .1}}, {{"index", 1}, {"normalized", .9}}}}})["ok"] == true);
      CHECK(jsfx_values[0] == .1 && jsfx_values[1] == .9);
      CHECK(edit("endParamEditGroup", {{"gestureId", group}})["data"]["ended"] == true);
      CHECK(jsfx_undo_begin == 1 && jsfx_undo_end == 1 && jsfx_ends == 2);
      CHECK(edit("setParam", {{"index", 0}, {"normalized", .3}})["data"]["parameter"]["normalized"] == .3);
      CHECK(jsfx_undo_begin == 2 && jsfx_undo_end == 2);
      CHECK(edit("setParam", {{"index", 0}, {"value", .4}, {"normalized", true}})["error"]["code"] == "INVALID_ARGUMENT");
      group = edit("beginParamEditGroup", {{"indices", {0, 1}}})["data"]["gestureId"];
      jsfx_fail_param = 1;
      const auto partial = edit("setParams", {{"gestureId", group}, {"values", {{{"index", 0}, {"normalized", .6}}, {{"index", 1}, {"normalized", .2}}}}});
      CHECK(partial["error"]["code"] == "PARTIAL_WRITE" && partial["error"]["details"]["applied"] == Json::array({0}));
      CHECK(jsfx_values[0] == .6 && jsfx_values[1] == .9 && jsfx_undo_begin == 3 && jsfx_undo_end == 3);
      jsfx_fail_param = -1;
      CHECK(edit("endParamEditGroup", {{"gestureId", group}})["data"]["ended"] == false);
      CHECK(result(runtime, *window, window->send("ReaWeb_ServiceSend", {"jsfx", "setParam", context}))["error"]["code"] == "INVALID_ARGUMENT");

      int reload_hello = 0;
      window->on_hide = [&] {
        window->document = "jsfx-reloaded";
        reload_hello = window->send("__reawebHello", {1});
      };
      window->options.on_message("__reawebDawKey:d"); runtime.tick();
      CHECK(keys == "drud");
      window->reload();
      CHECK(keys == "drudu");
      CHECK(!window->visible() && runtime.diagnostics(id)["jsfx"]["presented"] == false);
      result(runtime, *window, reload_hello);
      CHECK(runtime.is_ready(id));
      CHECK(call("present", {{"sessionId", session_id}, {"activation", first["activation"]}})["result"]["data"]["presented"] == false);
      const auto fresh = call("getParamState", {{"sessionId", session_id}})["result"]["data"];
      CHECK(fresh["activation"] > first["activation"] && std::stoull(fresh["revision"].get<std::string>()) > std::stoull(first["revision"].get<std::string>()));
      CHECK(fresh["sessionId"] == session_id);
      CHECK(call("present", {{"sessionId", session_id}, {"activation", fresh["activation"]}})["result"]["data"]["presented"] == true);
      CHECK(window->visible());
      auto floating_editor = std::make_shared<Editor>();
      jsfx_floating = floating_editor.get();
      discovery->editor = {&track, 0, true, jsfx_floating, floating_editor}; discovery->found = false;
      until(runtime, [&] { return discovery->found && runtime.diagnostics(id)["jsfx"]["cached"] == false; });
      CHECK(windows.size() == before + 1 && floating_editor->size_requests == 1);
      CHECK(floating_editor->width == 480 && floating_editor->height == 240 && floating_editor->exact);
      CHECK(call("present", {{"sessionId", session_id}, {"activation", fresh["activation"]}})["result"]["data"]["presented"] == false);
      const auto floating_state = call("getParamState", {{"sessionId", session_id}})["result"]["data"];
      CHECK(call("present", {{"sessionId", session_id}, {"activation", floating_state["activation"]}})["result"]["data"]["presented"] == true);
      jsfx_floating = nullptr; floating_editor->alive = false;
      discovery->editor = {&track, 0, false, editor.get(), editor}; discovery->found = false;
      until(runtime, [&] { return discovery->found && runtime.diagnostics(id)["jsfx"]["cached"] == false; });
      CHECK(windows.size() == before + 1 && editor->size_requests == 3);
      CHECK(call("present", {{"sessionId", session_id}, {"activation", floating_state["activation"]}})["result"]["data"]["presented"] == false);
      const auto returned = call("getParamState", {{"sessionId", session_id}})["result"]["data"];
      CHECK(call("present", {{"sessionId", session_id}, {"activation", returned["activation"]}})["result"]["data"]["presented"] == true);
      window->options.on_message("__reawebDawKey:d");
      editor->alive = false; runtime.tick();
      CHECK(keys == "drudu");
      CHECK(!window->visible() && runtime.diagnostics(id)["jsfx"]["cached"] == true);
      CHECK(!runtime.captures_keyboard(window.get(), [](void*, void*) { return false; }, true));
      CHECK(call("present", {{"sessionId", session_id}, {"activation", fresh["activation"]}})["result"]["data"]["presented"] == false);
    }
    windows.clear(); profiles.clear(); platform_count = 0; platform_pumps = 0;
    {
      int activations = 0;
      auto startup_docks = docks; startup_docks.activate = [&](void*) { ++activations; };
      Runtime runtime(host, root / "StartupProfile", [](const std::string&) {}, startup_docks);
      runtime.schedule_webview_warmup();
      CHECK(platform_count == 0);
      runtime.tick();
      CHECK(windows.empty());
#ifdef _WIN32
      CHECK(platform_count == 1);
      runtime.schedule_webview_warmup(); runtime.tick();
      CHECK(platform_count == 1);
#else
      CHECK(platform_count == 0);
#endif
      runtime.configure_webviews(5);
      runtime.tick();
      CHECK(platform_count == 1 && platform_capacity == 5 && platform_idles > 0 && windows.empty());
      const auto idles = platform_idles;
      runtime.configure_webviews(0); runtime.tick();
      CHECK(platform_capacity == 0 && platform_idles == idles);
      runtime.configure_webviews(5); runtime.tick();
      CHECK(platform_capacity == 5 && platform_idles > idles);
      bool invalid_capacity = false;
      try { runtime.configure_webviews(33); } catch (const Error& e) { invalid_capacity = e.code == "INVALID_ARGUMENT"; }
      CHECK(invalid_capacity && platform_capacity == 5);
      defer_next_presentation = true;
      const auto id = runtime.open(entry.u8string()); auto window = windows.back().lock();
      const auto busy_idles = platform_idles;
      runtime.tick(); CHECK(platform_idles == busy_idles);
      result(runtime, *window, window->send("__reawebHello", {1}));
      CHECK(runtime.is_ready(id) && !window->visible());
      runtime.tick(); CHECK(platform_idles == busy_idles);
      result(runtime, *window, window->send("ReaWeb_SetBounds", {{{"width", 900}}}));
      CHECK(window->wants_visible && !window->visible());
      runtime.set_docked(id, true);
      CHECK(activations == 0 && window->wants_visible);
      result(runtime, *window, window->send("ReaWeb_SetVisible", {false}));
      CHECK(!window->wants_visible);
      result(runtime, *window, window->send("ReaWeb_SetVisible", {true}));
      CHECK(activations == 1 && window->wants_visible && window->visible());
      runtime.close(id);

      defer_next_presentation = true;
      const auto closing = runtime.open(entry.u8string()); auto pending = windows.back().lock();
      result(runtime, *pending, pending->send("__reawebHello", {1}));
      result(runtime, *pending, pending->send("ReaWeb_LifecycleSubscribe", {true}));
      CHECK(pending->options.on_reload());
      runtime.close(closing);
      CHECK(pending->presentation_cancelled && !pending->visible());
      until(runtime, [&] { return !runtime.is_open(closing); });
      CHECK(pending->reloads == 0);
    }
    windows.clear(); profiles.clear(); platform_count = 0; platform_pumps = 0;
    {
      Runtime runtime(host, root / "FramelessProfile", [](const std::string&) {}, docks);
      for (const auto* invalid : {R"({"frameless":1})", R"({"resizable":null})", R"({"frameless":"true"})", R"({"transparent":1})", R"({"transparent":null})", R"({"transparent":"true"})"}) {
        bool rejected = false;
        try { runtime.open_ex(entry.u8string(), invalid); } catch (const Error& error) { rejected = error.code == "INVALID_ARGUMENT"; }
        CHECK(rejected);
      }
      const auto id = runtime.open_ex(entry.u8string(), R"({"instanceKey":"frame","hidden":true,"frameless":true,"resizable":false})");
      auto window = windows.back().lock();
      CHECK(window->options.frameless && !window->options.resizable && !window->shown);
      CHECK(runtime.open_ex("missing.html", R"({"instanceKey":"frame","hidden":true,"frameless":false,"resizable":true})") == id);
      CHECK(window->options.frameless && !window->options.resizable && window->reloads == 0);
      result(runtime, *window, window->send("__reawebHello", {1}));
      auto state = [&] { return result(runtime, *window, window->send("ReaWeb_GetWindowState"))["result"]; };
      CHECK(state()["frameless"] == true && state()["resizable"] == false && state()["framelessActive"] == true);
      runtime.set_docked(id, true);
      CHECK(state()["framelessActive"] == false);
      CHECK(result(runtime, *window, window->send("ReaWeb_SetFrameless", {false}))["result"]["docked"] == true);
      CHECK(state()["frameless"] == false && !window->options.frameless);
      CHECK(result(runtime, *window, window->send("ReaWeb_SetCornerRadius", {24.5}))["result"]["cornerRadius"] == 24.5);
      CHECK(state()["cornerRadiusActive"] == false);
      result(runtime, *window, window->send("ReaWeb_SetFrameless", {true}));
      runtime.show(id); runtime.set_docked(id, false);
      CHECK(state()["framelessActive"] == true && window->reloads == 0);
      CHECK(state()["cornerRadiusActive"] == true && window->radius == 24.5);
      const auto original_bounds = window->bounds;
      for (bool value : {false, true, false, true}) {
        CHECK(result(runtime, *window, window->send("ReaWeb_SetFrameless", {value}))["result"]["frameless"] == value);
        CHECK(window->bounds == original_bounds && window->reloads == 0);
      }
      for (const auto& invalid : Json::array({-1, 65, "12", nullptr, true}))
        CHECK(result(runtime, *window, window->send("ReaWeb_SetCornerRadius", Json::array({invalid})))["error"]["code"] == "INVALID_ARGUMENT");
      CHECK(result(runtime, *window, window->send("ReaWeb_SetFrameless", {1}))["error"]["code"] == "INVALID_ARGUMENT");
      CHECK(result(runtime, *window, window->send("ReaWeb_SetCornerRadius", {0}))["result"]["cornerRadiusActive"] == false);
      CHECK(result(runtime, *window, window->send("ReaWeb_SetBounds", {{{"width", 432}}}))["result"]["width"] == 432);
      const auto child = result(runtime, *window, window->send("ReaWeb_OpenEx", {"index.html", {{"frameless", true}, {"resizable", false}}}))["result"].get<int>();
      CHECK(child != id && windows.back().lock()->options.frameless && !windows.back().lock()->options.resizable);
      CHECK(result(runtime, *window, window->send("ReaWeb_OpenEx", {"index.html", {{"resizable", 0}}}))["error"]["code"] == "INVALID_ARGUMENT");
      runtime.close(child); runtime.close(id);
      until(runtime, [&] { return runtime.diagnostics(id).value("stage", "") == "closed"; });
      const auto reopened = runtime.open_ex(entry.u8string(), R"({"instanceKey":"frame","hidden":true})");
      CHECK(!windows.back().lock()->options.frameless && windows.back().lock()->options.resizable);
      runtime.close(reopened);
    }
    windows.clear(); profiles.clear(); platform_count = 0; platform_pumps = 0;
    {
      int activations = 0;
      auto alpha_docks = docks; alpha_docks.activate = [&](void*) { ++activations; };
      Runtime runtime(host, root / "TransparentProfile", [&](const std::string& error) { errors.push_back(error); }, alpha_docks);
      const auto original = runtime.open(entry.u8string());
      auto first = windows.back().lock();
      runtime.set_docked(original, true); runtime.close(original);
      until(runtime, [&] { return runtime.diagnostics(original).value("stage", "") == "closed"; });
      const auto before = activations;
      const auto id = runtime.open_ex(entry.u8string(), R"({"instanceKey":"alpha","hidden":true,"frameless":true,"transparent":true})");
      auto window = windows.back().lock();
      CHECK(window->options.transparent && !window->shown && !runtime.is_docked(id) && activations == before);
      CHECK(runtime.open_ex("missing.html", R"({"instanceKey":"alpha","hidden":true,"transparent":false})") == id);
      CHECK(window->options.transparent && window->reloads == 0);
      result(runtime, *window, window->send("__reawebHello", {1}));
      const auto state = [&] { return result(runtime, *window, window->send("ReaWeb_GetWindowState"))["result"]; };
      CHECK(state()["transparent"] == true && state()["transparentActive"] == true);
      bool refused = false;
      try { runtime.set_docked(id, true); } catch (const Error& error) { refused = error.code == "DOCK_UNAVAILABLE"; }
      CHECK(refused && !runtime.set_docked(id, false) && !runtime.is_docked(id));
      CHECK(result(runtime, *window, window->send("ReaWeb_SetDocked", {true}))["error"]["code"] == "DOCK_UNAVAILABLE");
      window->options.on_dock_toggle(); runtime.tick();
      CHECK(!runtime.is_docked(id) && activations == before && window->reloads == 0);
      runtime.show(id);
      CHECK(window->shown && state()["transparentActive"] == true && activations == before);
      result(runtime, *window, window->send("ReaWeb_SetVisible", {false}));
      CHECK(state()["transparentActive"] == true);
      CHECK(result(runtime, *window, window->send("ReaWeb_OpenEx", {"index.html", {{"transparent", "yes"}}}))["error"]["code"] == "INVALID_ARGUMENT");
      const auto child = result(runtime, *window, window->send("ReaWeb_OpenEx", {"index.html", {{"transparent", true}, {"hidden", true}}}))["result"].get<int>();
      CHECK(windows.back().lock()->options.transparent);
      runtime.close(child); runtime.close(id);
      until(runtime, [&] { return runtime.diagnostics(id).value("stage", "") == "closed"; });
      const auto restored = runtime.open(entry.u8string());
      CHECK(!windows.back().lock()->options.transparent && runtime.is_docked(restored));
      runtime.close(restored);
      until(runtime, [&] { return runtime.diagnostics(restored).value("stage", "") == "closed"; });
      const auto failed = runtime.open_ex(entry.u8string(), R"({"transparent":true,"hidden":true})");
      auto pending = windows.back().lock();
      pending->options.on_error("TRANSPARENCY_FAILED: injected native initialization failure");
      CHECK(!runtime.is_open(failed) && pending->presentation_cancelled);
      until(runtime, [&] { return runtime.diagnostics(failed).value("stage", "") == "failed"; });
      CHECK(runtime.diagnostics(failed)["lastError"] == "TRANSPARENCY_FAILED: injected native initialization failure");
      fail_next_presentation = true;
      refused = false;
      try { runtime.open_ex(entry.u8string(), R"({"transparent":true})"); }
      catch (const Error& error) { refused = error.code == "TRANSPARENCY_FAILED"; }
      CHECK(refused && windows.back().expired());
      errors.clear();
    }
    windows.clear(); profiles.clear(); platform_count = 0; platform_pumps = 0;
    {
      int activations = 0;
      auto background_docks = docks; background_docks.activate = [&](void*) { ++activations; };
      Runtime runtime(host, root / "HiddenProfile", [](const std::string&) {}, background_docks);
      for (const auto* invalid : {"", "null", "[]", "{", "{\"hidden\":1}", "{\"multiple\":null}",
           "{\"instanceKey\":false}", "{\"id\":0}", "{\"unknown\":true}", "{\"id\":\"a\\u0000b\"}"}) {
        bool rejected = false;
        try { runtime.open_ex(entry.u8string(), invalid); }
        catch (const Error& error) { rejected = error.code == "INVALID_ARGUMENT"; }
        CHECK(rejected);
      }
      const auto options = R"({"instanceKey":"hidden-launcher","id":"main","hidden":true})";
      const auto id = runtime.open_ex(entry.u8string(), options);
      auto window = windows.back().lock();
      CHECK(window->options.hidden && !window->shown && window->focuses == 0);
      CHECK(runtime.open_ex("missing.html", options) == id && window->focuses == 0);
      result(runtime, *window, window->send("__reawebHello", {1}));
      CHECK(runtime.is_ready(id) && !window->shown);
      CHECK(runtime.send(id, "preload"));
      result(runtime, *window, window->send("ReaWeb_HostSend", {"ui-ready"}));
      CHECK(runtime.receive(id) == "ui-ready" && !window->shown);
      CHECK(runtime.set_docked(id, true) && runtime.is_docked(id));
      CHECK(!docked.count(window.get()) && activations == 0);
      window->send("ReaWeb_Reload");
      until(runtime, [&] { return window->reloads == 1; });
      CHECK(!window->shown && window->reloads == 1);
      window->document = "reloaded";
      result(runtime, *window, window->send("__reawebHello", {1}));
      const auto child = result(runtime, *window, window->send("ReaWeb_OpenEx", {"index.html", {{"hidden", true}}}))["result"].get<int>();
      CHECK(child != id && windows.back().lock()->options.hidden && !windows.back().lock()->shown);
      CHECK(result(runtime, *window, window->send("ReaWeb_OpenEx", {"index.html", {{"multiple", true}}}))["error"]["code"] == "INVALID_ARGUMENT");
      CHECK(runtime.show(id) && window->shown && window->focuses == 0 && docked.count(window.get()));
      CHECK(runtime.open_ex("missing.html", options) == id && window->shown && window->focuses == 0);
      result(runtime, *window, window->send("ReaWeb_SetVisible", {false}));
      CHECK(runtime.open_instance("missing.html", "hidden-launcher", "main") == id && window->shown && window->focuses == 1);
      result(runtime, *window, window->send("ReaWeb_SetVisible", {false}));
      CHECK(runtime.open_ex("missing.html", R"({"instanceKey":"hidden-launcher","id":"main","hidden":false})") == id && window->shown);
      const auto many = runtime.open_ex(entry.u8string(), R"({"instanceKey":"hidden-launcher","id":"main","hidden":true,"multiple":true})");
      CHECK(many != id && !windows.back().lock()->shown);
      runtime.close(many); runtime.close(child); runtime.close(id);
      until(runtime, [&] { return runtime.diagnostics(id).value("stage", "") == "closed"; });
      window.reset();
      const auto restored = runtime.open_ex(entry.u8string(), options);
      auto pending = windows.back().lock();
      CHECK(runtime.is_docked(restored) && !docked.count(pending.get()) && !pending->shown);
      CHECK(runtime.show(restored) && docked.count(pending.get()) && pending->shown);
      runtime.close(restored);
      CHECK(!runtime.focus(restored));
      bool closed_rejected = false;
      try { runtime.show(restored); } catch (const Error& error) { closed_rejected = error.code == "WINDOW_CLOSED"; }
      CHECK(closed_rejected);
    }
    windows.clear(); profiles.clear(); platform_count = 0; platform_pumps = 0;
    {
      Runtime runtime(host, root, [&](const std::string& error) { errors.push_back(error); }, docks);
      const auto id = runtime.open("Tool/index.html"); auto first = windows.back().lock();
      CHECK(runtime.is_open(id) && !runtime.is_ready(id));
      CHECK(result(runtime, *first, first->send("CountTracks", {0}))["error"]["code"] == "DOCUMENT_STALE");
      CHECK(calls == 0);
      CHECK(result(runtime, *first, first->send("__reawebHello", {1}))["result"]["protocol"] == 1);
      result(runtime, *first, first->send("ReaWeb_Subscribe", {"native-timer"}));
      const auto timer = result(runtime, *first, first->send("ReaWeb_TimerStart", Json::array({Json{{"delay", 10}, {"interval", 20}}})))["result"];
      auto timer_events = [&] { int count = 0; for (const auto& event : first->responses)
        if (event.value("event", "") == "native-timer" && event["data"]["id"] == timer) ++count; return count; };
      until(runtime, [&] { return timer_events() >= 3; });
      result(runtime, *first, first->send("ReaWeb_TimerStop", Json::array({timer})));
      CHECK(result(runtime, *first, first->send("ReaWeb_TimerStart", Json::array({Json{{"intervalMs", 20}}})))["error"]["code"] == "INVALID_ARGUMENT");
      result(runtime, *first, first->send("ReaWeb_Unsubscribe", {"native-timer"}));
      CHECK(runtime.is_ready(id));
      CHECK(result(runtime, *first, first->send("ReaWeb_ServiceInvoke", {"runtime", "getInfo", nullptr}))["result"]["serviceABI"] == 1);
      CHECK(result(runtime, *first, first->send("ReaWeb_ServiceInvoke", {"missing", "ping", nullptr}))["error"]["code"] == "SERVICE_NOT_FOUND");
      CHECK(result(runtime, *first, first->send("ReaWeb_ServiceInvoke", {"runtime", "missing", nullptr}))["error"]["code"] == "METHOD_NOT_FOUND");
      CHECK(result(runtime, *first, first->send("ReaWeb_ServiceSubscribe", {"runtime", "changed"}))["result"].contains("handle"));
      const auto runtime_service = runtime.services().lookup("runtime");
      CHECK(runtime.services().emit(runtime_service, id, "changed", "{\"ok\":true}") == REAWEB_OK);
      until(runtime, [&] { for (const auto& event : first->responses) if (event.value("serviceEvent", "") == "changed") return true; return false; });
      CHECK(result(runtime, *first, first->send("ReaWeb_ServiceUnsubscribe", {"runtime", "changed"})).contains("result"));
      CHECK(result(runtime, *first, first->send("ReaWeb_ServiceUnsubscribe", {"runtime", "changed"})).contains("result"));
      CHECK(result(runtime, *first, first->send("ReaWeb_ServiceInvoke", {"runtime", "getInfo"}))["error"]["code"] == "INVALID_ARGUMENT");
      struct ServiceProbe { uint64_t request = 0; int cancelled = 0, inputs = 0, shutdowns = 0; } probe;
      ReaWeb_ServiceCallbacks callbacks{sizeof(callbacks), REAWEB_SERVICE_ABI, &probe,
        [](void* context, uint64_t, uint64_t request, int, const char* method, const char*) {
          if (std::string(method) == "input") { ++static_cast<ServiceProbe*>(context)->inputs; return int(REAWEB_OK); }
          static_cast<ServiceProbe*>(context)->request = request; return int(REAWEB_OK);
        }, [](void* context, uint64_t) { ++static_cast<ServiceProbe*>(context)->cancelled; }};
      uint64_t service_handle = 0;
      CHECK(runtime.services().add("probe", &callbacks, &service_handle) == REAWEB_OK);
      CHECK(runtime.services().set_input(service_handle, "input") == REAWEB_OK);
      CHECK(runtime.services().set_shutdown(service_handle, [](void* context) { ++static_cast<ServiceProbe*>(context)->shutdowns; }) == REAWEB_OK);
      const auto queued_slow = first->send("ReaWeb_ServiceInvoke", {"runtime", "getInfo", nullptr}, 0, 0, 5000);
      const auto immediate = first->send("ReaWeb_ServiceSend", {"probe", "input", {{"mask", 1}}});
      CHECK(probe.inputs == 1 && first->response(queued_slow).is_null());
      CHECK(result(runtime, *first, immediate)["result"] == true);
      CHECK(result(runtime, *first, queued_slow).contains("result"));
      const auto expired_input = first->send("ReaWeb_ServiceSend", {"probe", "input", nullptr}, 0, 1);
      CHECK(result(runtime, *first, expired_input)["error"]["code"] == "REQUEST_EXPIRED" && probe.inputs == 1);
      const auto service_window = runtime.open("Tool/index.html"); auto service_page = windows.back().lock();
      result(runtime, *service_page, service_page->send("__reawebHello", {1}));
      auto pending_service = service_page->send("ReaWeb_ServiceInvoke", {"probe", "pending", nullptr});
      until(runtime, [&] { return probe.request != 0; });
      service_page->options.on_navigation();
      CHECK(probe.cancelled == 1);
      CHECK(runtime.services().complete(service_handle, probe.request, "null", 0, nullptr) == REAWEB_REQUEST_GONE);
      CHECK(service_page->response(pending_service).is_null());
      service_page->document = "service-reload";
      result(runtime, *service_page, service_page->send("__reawebHello", {1}));
      probe.request = 0;
      service_page->send("ReaWeb_ServiceInvoke", {"probe", "pending", nullptr});
      until(runtime, [&] { return probe.request != 0; });
      CHECK(runtime.close(service_window)); runtime.tick();
      CHECK(probe.cancelled == 2);
      CHECK(runtime.services().complete(service_handle, probe.request, "null", 0, nullptr) == REAWEB_REQUEST_GONE);
      probe.request = 0;
      pending_service = first->send("ReaWeb_ServiceInvoke", {"probe", "pending", nullptr});
      until(runtime, [&] { return probe.request != 0; });
      CHECK(runtime.services().remove(service_handle) == REAWEB_OK);
      CHECK(probe.shutdowns == 1);
      CHECK(result(runtime, *first, pending_service)["error"]["code"] == "EXTENSION_UNLOADED");
      CHECK(probe.cancelled == 3);
      CHECK(result(runtime, *first, first->send("ReaWeb_SetIconVisible", {false}))["result"] == true);
      CHECK(!first->icon_visible && first->icons.empty() && !runtime.is_docked(id));
      CHECK(runtime.diagnostics(id)["window"]["iconVisible"] == false);
      CHECK(result(runtime, *first, first->send("ReaWeb_SetIconVisible", {true}))["result"] == true);
      CHECK(first->icon_visible && first->icons.empty());
      const auto web = result(runtime, *first, first->send("ReaWeb_GetCapabilities"))["result"]["webRuntime"];
      CHECK(web["contract"] == 1 && web["mode"] == "app-virtual" && web["storageIsolation"] == "origin");
      CHECK(first->options.url == web["origin"].get<std::string>() + "/index.html");
      CHECK(profiles.back() == root / "ReaWebAPI" / "WebViewData");
      const auto app = result(runtime, *first, first->send("ReaWeb_GetAppInfo"))["result"];
      CHECK(app["id"] == web["appId"] && app["name"] == "Tool" && app["version"].is_null());
      CHECK(first->options.app_name() == app["name"]);
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
      CHECK(!runtime.captures_keyboard(first.get(), [](void*, void*) { return false; }, true));
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
      const auto second_id = result(runtime, *first, first->send("ReaWeb_Open", {"index.html"}))["result"].get<int>();
      CHECK(windows.size() == 3 && platform_count == 1);
      auto second = windows.back().lock();
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
      CHECK(result(runtime, *window, window->send("ReaWeb_MoveFile", {"async.txt", "moved.txt"}))["result"]["bytes"] == 11);
      CHECK(result(runtime, *window, window->send("ReaWeb_ReadFile", {"moved.txt"}))["result"] == "worker data");
      CHECK(result(runtime, *window, window->send("ReaWeb_Stat", {"async.txt"}))["result"]["exists"] == false);
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
      std::ofstream(root / "Scripts" / "Other" / "app.json") << R"({"id":"other"})";
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
      // connect() points the shared adapter at this block's host. Leaving it
      // there would hand every later block a pointer to a destroyed object.
      adapter::host = &host;
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
      // Continuous traffic in older windows must not starve later windows.
      Runtime runtime(host, root, [](const std::string&) {}, docks);
      std::vector<int> ids;
      std::vector<std::shared_ptr<FakeWindow>> views;
      for (int i = 0; i < 32; ++i) {
        ids.push_back(runtime.open("Tool/index.html")); views.push_back(windows.back().lock());
        result(runtime, *views.back(), views.back()->send("__reawebHello", {1}));
        result(runtime, *views.back(), views.back()->send("ReaWeb_Subscribe", {"message"}));
        views.back()->message_delay = std::chrono::milliseconds(3);
      }
      auto count = [](const FakeWindow& view) {
        return std::count_if(view.responses.begin(), view.responses.end(), [](const Json& r) { return r.value("event", "") == "message"; });
      };
      for (int pass = 0; pass < 96; ++pass) {
        for (size_t i = 0; i < ids.size(); ++i) runtime.send(ids[i], std::to_string(ids[i]));
        runtime.tick();
      }
      for (size_t i = 0; i < ids.size(); ++i) {
        CHECK(count(*views[i]) > 0);
        for (const auto& r : views[i]->responses)
          if (r.value("event", "") == "message") CHECK(r.at("data") == std::to_string(ids[i]));
      }
      // Removal of the last serviced window must not invalidate the output cursor.
      for (int i : {3, 0, 6, 1, 7, 2, 5, 4}) runtime.close(ids[i]);
      for (size_t i = 8; i < ids.size(); ++i) runtime.close(ids[i]);
      runtime.tick();
      const auto fresh = runtime.open("Tool/index.html"); auto view = windows.back().lock();
      result(runtime, *view, view->send("__reawebHello", {1}));
      result(runtime, *view, view->send("ReaWeb_Subscribe", {"message"}));
      view->tick_delay = std::chrono::milliseconds(3);
      runtime.send(fresh, "fresh");
      until(runtime, [&] { return count(*view) == 1; });
      CHECK(runtime.receive(fresh).empty());
    }
    {
      // Frame sampling can exceed the bridge budget on a busy host.
      auto slow_host = host;
      slow_host.native_function = [](const char* name) -> void* {
        if (std::string(name) != "GetPlayState") return nullptr;
        return reinterpret_cast<void*>(+[]() -> int {
          std::this_thread::sleep_for(std::chrono::milliseconds(3)); return 0;
        });
      };
      Runtime runtime(slow_host, root, [](const std::string&) {}, docks);
      std::vector<int> ids, replies;
      std::vector<std::shared_ptr<FakeWindow>> views;
      for (int i = 0; i < 8; ++i) {
        ids.push_back(runtime.open("Tool/index.html")); views.push_back(windows.back().lock());
        result(runtime, *views.back(), views.back()->send("__reawebHello", {1}));
      }
      for (size_t i = 0; i < ids.size(); ++i)
        replies.push_back(views[i]->send("ReaWeb_HostSend", {std::to_string(ids[i])}, 0, 0, 8192));
      until(runtime, [&] {
        for (size_t i = 0; i < ids.size(); ++i) if (views[i]->response(replies[i]).is_null()) return false;
        return true;
      });
      for (auto id : ids) CHECK(runtime.receive(id) == std::to_string(id));
    }
    for (size_t payload : {size_t{0}, size_t{8192}}) {
      // Replaceable frames must not build a FIFO backlog ahead of host messages.
      auto owner = std::make_unique<Runtime>(host, root, [](const std::string&) {}, docks);
      auto& runtime = *owner;
      std::vector<int> ids;
      std::vector<std::shared_ptr<FakeWindow>> views;
      for (int i = 0; i < 8; ++i) {
        ids.push_back(runtime.open("Tool/index.html")); views.push_back(windows.back().lock());
        result(runtime, *views.back(), views.back()->send("__reawebHello", {1}));
        result(runtime, *views.back(), views.back()->send("ReaWeb_Subscribe", {"message"}));
      }
      for (auto& view : views) result(runtime, *view, view->send("ReaWeb_Subscribe", {"frame"}));
      views.front()->tick_delay = std::chrono::milliseconds(3);
      for (auto& view : views) view->frame_delay = std::chrono::milliseconds(1);
      const std::string block(payload, 'x'), latest = block + "latest";
      for (int pass = 0; pass < 32; ++pass) {
        for (auto id : ids) runtime.set_frame(id, block + std::to_string(pass));
        runtime.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      for (auto id : ids) runtime.send(id, "latency-probe");
      for (int pass = 0; pass < 32; ++pass) {
        for (auto id : ids) runtime.set_frame(id, latest);
        runtime.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      for (auto& view : views) {
        CHECK(std::any_of(view->responses.begin(), view->responses.end(), [](const Json& r) {
          return r.value("event", "") == "message" && r.at("data") == "latency-probe";
        }));
        CHECK(std::any_of(view->responses.begin(), view->responses.end(), [&](const Json& r) {
          return r.value("event", "") == "frame" && r.at("data").value("lua", "") == latest;
        }));
      }
      auto view = views.back();
      view->options.on_navigation(); view->document = "frame-reloaded";
      result(runtime, *view, view->send("__reawebHello", {1}));
      result(runtime, *view, view->send("ReaWeb_Subscribe", {"frame"}));
      runtime.set_frame(ids.back(), latest);
      until(runtime, [&] {
        return std::any_of(view->responses.begin(), view->responses.end(), [&](const Json& r) {
          return r.value("document", "") == view->document && r.value("event", "") == "frame" && r.at("data").value("lua", "") == latest;
        });
      });
    }
    {
      // A request the page posted between two timer passes must reach Lua in the
      // next ReaWeb_Receive call itself. When only tick() pumped the WebView IPC
      // and dispatched, the reply had to wait for the pass after that, which made
      // page to Lua one whole pass slower than Lua to page.
      Runtime runtime(host, root, [](const std::string&) {}, docks);
      const auto id = runtime.open("Tool/index.html");
      auto window = windows.back().lock();
      result(runtime, *window, window->send("__reawebHello", {1}));
      const auto pumps_before = platform_pumps;
      window->send("ReaWeb_HostSend", {"posted between passes"});
      CHECK(runtime.receive(id) == "posted between passes");
      CHECK(platform_pumps == pumps_before + 1);
      // A Lua pass polls up to 32 times in a row. Only the first of them may
      // pump, or the platform would repeat its window synchronisation 32 times
      // in one pass.
      CHECK(runtime.receive(id).empty());
      CHECK(platform_pumps == pumps_before + 1);
      runtime.tick();
      CHECK(platform_pumps == pumps_before + 2);
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
        auto reply = result(runtime, *window, window->send("ReaWeb_DocumentTitle", {text}));
        CHECK(window->options.app_name() == (window->options.title == fallback ? "Tool" : window->options.title));
        return reply;
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
    {
      const auto resource = root / "PrivateState";
      const auto platforms_before = platform_count;
      std::vector<fs::path> entries, records;
      std::vector<std::string> origins;
      for (const auto& name : {"AppA", "AppB"}) {
        auto entry = resource / "Scripts" / name / "index.html";
        fs::create_directories(entry.parent_path()); std::ofstream(entry) << "<html></html>";
        std::ofstream(entry.parent_path() / "app.json") << Json{{"id", "test-" + state_key(entry.parent_path().u8string(), 0)}};
        entries.push_back(entry);
        records.push_back(resource / "ReaWebAPI" / "Apps" / app_identity(entry.parent_path()));
      }
      {
        Runtime runtime(host, resource, [&](const std::string& error) { errors.push_back(error); });
        for (size_t i = 0; i < entries.size(); ++i) {
          runtime.open(entries[i].u8string());
          auto window = windows.back().lock();
          result(runtime, *window, window->send("__reawebHello", {1}));
          origins.push_back(result(runtime, *window, window->send("ReaWeb_GetCapabilities"))["result"]["webRuntime"]["origin"]);
          CHECK(fs::is_regular_file(records[i] / "origin.json"));
          const auto app = result(runtime, *window, window->send("ReaWeb_GetAppInfo"))["result"];
          CHECK(fs::u8path(app["dataPath"].get<std::string>()) == records[i] / "Data");
          CHECK(result(runtime, *window, window->send("ReaWeb_WriteFile", {(records[i] / "Data" / "setting.txt").u8string(), std::to_string(i)}))["result"]["bytes"] == 1);
          window->bounds["x"] = 200 + i;
          window->inspector.restore({{"mode", i ? "floating" : "embedded"}, {"widthRatio", 0.5}});
        }
        CHECK(origins[0] != origins[1]);
        runtime.open_dev("http://127.0.0.1:5173", entries[0].parent_path());
        CHECK(platform_count == platforms_before + 1);
        CHECK(profiles.back() == resource / "ReaWebAPI" / "WebViewData");
        CHECK(fs::is_directory(profiles.back()));
        const auto pumps_before = platform_pumps;
        runtime.tick();
        CHECK(platform_pumps == pumps_before + 1);
      }
      CHECK(!fs::exists(resource / "ReaWebAPI" / "WindowState"));
      {
        Runtime runtime(host, resource, [&](const std::string& error) { errors.push_back(error); });
        for (size_t i = 0; i < entries.size(); ++i) {
          const auto state = records[i] / "WindowState" / (state_key(entries[i].generic_u8string(), 0) + ".json");
          CHECK(fs::is_regular_file(state));
          runtime.open(entries[i].u8string());
          auto window = windows.back().lock();
          CHECK(window->bounds["x"] == 200 + i && window->inspector.floating == bool(i));
          result(runtime, *window, window->send("__reawebHello", {1}));
          CHECK(result(runtime, *window, window->send("ReaWeb_GetCapabilities"))["result"]["webRuntime"]["origin"] == origins[i]);
          std::ifstream input(records[i] / "Data" / "setting.txt"); std::string value; input >> value;
          CHECK(value == std::to_string(i));
        }
      }
      CHECK(platform_count == platforms_before + 2);
      for (const auto& app : fs::directory_iterator(resource / "ReaWebAPI" / "Apps"))
        CHECK(!fs::exists(app.path() / "WebViewData"));
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
    {
      const auto fallback_root = root / "FallbackIdentity";
      fs::create_directories(fallback_root);
      std::ofstream(fallback_root / "index.html") << "<html></html>";
      std::ofstream(fallback_root / "Unrelated.lua") << "-- This filename must not define identity";
      Runtime runtime(host, root, [](const std::string&) {}, docks);
      const auto id = runtime.open_instance((fallback_root / "index.html").u8string(), "@/Scripts/zaibuyidao_ReaGBA.lua", "", true);
      auto window = windows.back().lock();
      CHECK(window->options.url == "reaweb://zaibuyidao-reagba/index.html");
      result(runtime, *window, window->send("__reawebHello", {1}));
      CHECK(result(runtime, *window, window->send("ReaWeb_GetAppInfo"))["result"]["id"] == "zaibuyidao-reagba");
      const auto child = result(runtime, *window, window->send("ReaWeb_Open", {"index.html"}));
      CHECK(child.contains("result") && windows.back().lock()->options.url == window->options.url);
      const auto duplicate = root / "FallbackCopy";
      fs::create_directories(duplicate); std::ofstream(duplicate / "index.html") << "<html></html>";
      bool conflict = false;
      try { runtime.open_instance((duplicate / "index.html").u8string(), "@/Other/zaibuyidao_ReaGBA.lua"); }
      catch (const Error& error) { conflict = error.code == "APP_ID_CONFLICT"; }
      CHECK(conflict && runtime.is_open(id));
      std::ofstream(duplicate / "app.json") << R"({"id":"manifest-wins","name":"Display only"})";
      runtime.open_instance((duplicate / "index.html").u8string(), "opaque key", "", true);
      CHECK(windows.back().lock()->options.url == "reaweb://manifest-wins/index.html");
    }
    fs::create_directories(metadata_root);
    std::ofstream(metadata_root / "app.json") << R"({"id":"sendflow","name":"SendFlow","version":"1.2.3-beta.1"})";
    const auto metadata = app_info(metadata_root, root / "MetadataData", "stable-id");
    CHECK(metadata["name"] == "SendFlow" && metadata["version"] == "1.2.3-beta.1" && metadata["id"] == "stable-id");
    std::ofstream(metadata_root / "index.html") << "<title>Different page title</title>";
    {
      Runtime runtime(host, root, [](const std::string&) {}, docks);
      runtime.open((metadata_root / "index.html").u8string());
      CHECK(windows.back().lock()->options.app_name() == "SendFlow");
    }
    {
      const auto entry = root / "ReaGBA" / "web" / "index.html";
      fs::create_directories(entry.parent_path());
      std::ofstream(entry) << "<title>ReaGBA</title>";
      std::ofstream(entry.parent_path() / "app.json") << R"({"id":"reagba"})";
      Runtime runtime(host, root, [](const std::string&) {}, docks);
      const auto id = runtime.open(entry.u8string());
      auto window = windows.back().lock();
      result(runtime, *window, window->send("__reawebHello", {1}));
      result(runtime, *window, window->send("ReaWeb_DocumentTitle", {"ReaGBA"}));
      CHECK(window->options.app_name() == "ReaGBA");
      runtime.set_docked(id, true);
      CHECK(window->options.app_name() == "ReaGBA");
      result(runtime, *window, window->send("ReaWeb_SetTitle", {"Rea&GBA 音"}));
      CHECK(window->options.app_name() == "Rea&GBA 音");
      runtime.set_docked(id, false);
      CHECK(window->options.app_name() == "Rea&GBA 音");
    }

    {
      // A slow accessor must not consume the budget for stream tickets and detach replies.
      adapter::Host audio_host;
      audio_host.current_project = [&]() -> void* { return &storage; };
      audio_host.change_count = [](void*) { return 0; };
      audio_host.track_count = [] { return 1; };
      audio_host.track_identity = [](int) { return "test-track"; };
      audio_host.track_guid = [](void*) { return Guid{}; };
      audio_host.get_track = [&](void*, int) -> void* { return &track; };
      audio_host.get_selected_track = [&](void*, int) -> void* { return &track; };
      audio_host.valid_track = [&](void*, void* t) { return t == &track; };
      audio_host.play_state = [] { return 1; };
      audio_host.get_track_value = [](void*, const std::string& key) { return key == "I_NCHAN" ? 2.0 : 0.0; };
      const auto playback_start = std::chrono::steady_clock::now();
      audio_host.play_position = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - playback_start).count(); };
      audio_host.cursor_position = [] { return 0.0; };
      int reads = 0;
      static int* sample_reads;
      sample_reads = &reads;
      audio_host.native_function = [](const char* name) -> void* {
        if (!std::strcmp(name, "GetAudioDeviceInfo")) return nullptr;
        if (!std::strcmp(name, "CreateTrackAudioAccessor")) return reinterpret_cast<void*>(+[](void* t) { return t; });
        if (!std::strcmp(name, "DestroyAudioAccessor")) return reinterpret_cast<void*>(+[](void*) {});
        if (!std::strcmp(name, "AudioAccessorValidateState")) return reinterpret_cast<void*>(+[](void*) { return false; });
        if (!std::strcmp(name, "GetAudioAccessorSamples")) return reinterpret_cast<void*>(+[](void*, int, int channels, double, int frames, double* data) {
          ++*sample_reads;
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          std::fill(data, data + channels * frames, 0.0); return 1;
        });
        return adapter::resolve(name);
      };
      adapter::host = &audio_host;
      Runtime runtime(audio_host, root, [](const std::string&) {}, docks);
      const auto id = runtime.open(entry.u8string()); auto window = windows.back().lock();
      result(runtime, *window, window->send("__reawebHello", {1}));
      const auto name = runtime.producers().audio("meter", Json{{"source", "selected-track"}, {"updateRate", 120}}, id);
      const auto first_ticket = runtime.streams().attach(name, id, 0, "http://127.0.0.1:1234");
      runtime.producers().attached(name);
      runtime.producers().tick();
      const auto request = window->send("ReaWeb_StreamOpen", Json::array({name}));
      // Normal host cadence exceeds the sample interval, so every attached tick samples.
      auto advance = [&] { std::this_thread::sleep_for(std::chrono::milliseconds(15)); runtime.tick(); };
      for (int i = 0; i < 30 && window->response(request).is_null(); ++i) advance();
      CHECK(reads > 0);
      CHECK(window->response(request).contains("result"));
      const auto token = window->response(request)["result"]["token"];
      runtime.streams().detach(first_ticket["token"], id);
      const auto detach = window->send("ReaWeb_StreamDetach", Json::array({token}));
      for (int i = 0; i < 30 && window->response(detach).is_null(); ++i) advance();
      CHECK(window->response(detach)["result"] == true);
      advance();
      CHECK(runtime.streams().info()["streams"].empty());
      adapter::host = &host;
    }
    for (const auto& content : {"not json", "[]", R"({"name":4})", R"({"name":"   "})", R"({"version":"bad"})"}) {
      std::ofstream(metadata_root / "app.json") << content;
      bool rejected = false;
      try { app_info(metadata_root, root / "MetadataData", "stable-id"); }
      catch (const Error& error) { rejected = error.code == "APP_MANIFEST_INVALID"; }
      CHECK(rejected);
    }
    {
      const auto diagnostic_root = root / "diagnostic-profile";
      const auto target = "test-" + state_key(entry.parent_path().u8string(), 0);
      write_diagnostic_settings(diagnostic_root / "ReaWebAPI.ini", {true, false, target});
      Runtime runtime(host, diagnostic_root, [](const std::string&) {}, docks);
      const auto first = runtime.open(entry.u8string()); auto a = windows.back().lock();
      const auto second = runtime.open(entry.u8string()); auto b = windows.back().lock();
      result(runtime, *a, a->send("__reawebHello", {1}));
      result(runtime, *b, b->send("__reawebHello", {1}));
      CHECK(runtime.send(first, "secret-transport-body"));
      result(runtime, *a, a->send("ReaWeb_Log", {{{"level", "debug"}, {"message", "secret-js-body"}}}));
      a->options.on_error("secret-url?token=secret-token");
      runtime.tick();
      runtime.close(second); runtime.tick();
      std::ifstream file(diagnostic_root / "ReaWebAPI" / "Diagnostics" / Diagnostics::filename(target));
      std::string text{std::istreambuf_iterator<char>(file), {}};
      CHECK(text.find("secret-") == std::string::npos);
      std::set<int> ids; std::set<std::string> events;
      std::istringstream input(text); std::string line;
      while (std::getline(input, line)) {
        const auto record = Json::parse(line);
        ids.insert(record["windowId"].get<int>()); events.insert(record["event"].get<std::string>());
      }
      CHECK(ids.count(first) && ids.count(second));
      for (const auto* event : {"webview.create.begin", "webview.create.end", "webview.load.end", "lua.channel.begin",
          "javascript.log", "webview.load.fail", "instance.unbind", "webview.release.end"}) CHECK(events.count(event));
    }
    fs::remove_all(root);
    std::cout << "Runtime: handshake, worker dispatch, project/reload isolation, subscriptions, keyboard capture, queue limits and persistent docking passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
