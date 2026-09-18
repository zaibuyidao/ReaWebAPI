#include <reaper_plugin.h>
#include "runtime.hpp"
#include <cstring>
#include <atomic>
#include <memory>
#include <vector>

namespace {
using namespace reaweb;
std::unique_ptr<Runtime> runtime;
int (*register_api)(const char*, void*) = nullptr;
void (*console)(const char*) = nullptr;
std::string last_error;
std::vector<std::pair<std::string, void*>> registrations;
std::atomic<uint64_t> project_generation{0};
project_config_extension_t project_events{
  [](const char*, ProjectStateContext*, bool, project_config_extension_t*) { return false; },
  [](ProjectStateContext*, bool, project_config_extension_t*) {},
  [](bool is_undo, project_config_extension_t*) {
    // A file load can reuse the same ReaProject address. Undo is an edit, not a new project lifetime.
    if (!is_undo) project_generation.fetch_add(1, std::memory_order_relaxed);
  }, nullptr};

void log_error(const std::string& text) {
  last_error = text;
  if (console) console(("[ReaWebAPI] " + text + "\n").c_str());
}
template<class F> auto guarded(F&& fn, decltype(fn()) fallback) noexcept -> decltype(fn()) {
  try { return fn(); }
  catch (const std::exception& e) { log_error(e.what()); }
  catch (...) { log_error("Unexpected native exception"); }
  return fallback;
}

int ReaWebOpen(const char* path) {
  return guarded([&] { return runtime->open(path ? path : ""); }, 0);
}
bool ReaWeb_Close(int id) { return guarded([&] { return runtime->close(id); }, false); }
bool ReaWeb_IsOpen(int id) { return guarded([&] { return runtime->is_open(id); }, false); }
bool ReaWeb_DevTools(int id) { return guarded([&] { return runtime->devtools(id); }, false); }
bool ReaWeb_SetDocked(int id, bool docked) { return guarded([&] { return runtime->set_docked(id, docked); }, false); }
bool ReaWeb_IsDocked(int id) { return guarded([&] { return runtime->is_docked(id); }, false); }
bool ReaWeb_IsReady(int id) { return guarded([&] { return runtime->is_ready(id); }, false); }
bool ReaWeb_Focus(int id) { return guarded([&] { return runtime->focus(id); }, false); }
const char* ReaWeb_GetDiagnostics(int id) {
  static std::string value;
  return guarded([&]() -> const char* { value = runtime->diagnostics(id).dump(); return value.c_str(); }, "{}");
}
const char* ReaWeb_GetLastError() { return last_error.c_str(); }
void* open_vararg(void** args, int count) {
  return reinterpret_cast<void*>(static_cast<intptr_t>(ReaWebOpen(count >= 1 ? static_cast<const char*>(args[0]) : nullptr)));
}
template<bool (*Fn)(int)> void* id_vararg(void** args, int count) {
  const auto id = count >= 1 ? static_cast<int>(reinterpret_cast<intptr_t>(args[0])) : 0;
  return reinterpret_cast<void*>(static_cast<intptr_t>(Fn(id)));
}
void* error_vararg(void**, int) { return const_cast<char*>(ReaWeb_GetLastError()); }
void* diagnostics_vararg(void** args, int count) {
  return const_cast<char*>(ReaWeb_GetDiagnostics(count >= 1 ? static_cast<int>(reinterpret_cast<intptr_t>(args[0])) : 0));
}
int window_info(HWND window, INT_PTR type) {
  if (!runtime || (type != 0 && type != 1)) return 0;
  return guarded([&] { return runtime->captures_keyboard(window, [](void* parent, void* child) {
    return IsChild(static_cast<HWND>(parent), static_cast<HWND>(child)) != 0;
  }) ? 1 : 0; }, 0);
}
void* dock_vararg(void** args, int count) {
  if (count < 2) return nullptr;
  return reinterpret_cast<void*>(static_cast<intptr_t>(ReaWeb_SetDocked(
    static_cast<int>(reinterpret_cast<intptr_t>(args[0])), args[1] != nullptr)));
}
void timer() {
  try { runtime->tick(); }
  catch (const std::exception& e) { log_error(e.what()); }
  catch (...) { log_error("Unexpected native exception in timer"); }
}

void add_registration(const std::string& name, void* value) {
  if (!register_api(name.c_str(), value)) throw std::runtime_error("Failed to register " + name);
  registrations.emplace_back(name, value);
}
void add_api(const char* name, void* native, void* vararg, const char* definition) {
  add_registration(std::string("API_") + name, native);
  add_registration(std::string("APIvararg_") + name, vararg);
  add_registration(std::string("APIdef_") + name, const_cast<char*>(definition));
}
void unload() {
  for (auto it = registrations.rbegin(); it != registrations.rend(); ++it)
    register_api(("-" + it->first).c_str(), it->second);
  registrations.clear();
  runtime.reset();
  register_api = nullptr;
  console = nullptr;
}
template<class T> T load(reaper_plugin_info_t* rec, const char* name) {
  auto pointer = rec->GetFunc(name);
  if (!pointer) throw std::runtime_error(std::string("Required REAPER API is missing: ") + name);
  return reinterpret_cast<T>(pointer);
}
}

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t* rec) {
  if (!rec) { unload(); return 0; }
  if (rec->caller_version != REAPER_PLUGIN_VERSION || !rec->GetFunc || !rec->Register) return 0;
  register_api = rec->Register;
  try {
    console = load<void (*)(const char*)>(rec, "ShowConsoleMsg");
    auto resource = load<const char* (*)()>(rec, "GetResourcePath");
    auto enum_projects = load<ReaProject* (*)(int, char*, int)>(rec, "EnumProjects");
    auto count_tracks = load<int (*)(ReaProject*)>(rec, "CountTracks");
    auto count_selected = load<int (*)(ReaProject*)>(rec, "CountSelectedTracks");
    auto get_track = load<MediaTrack* (*)(ReaProject*, int)>(rec, "GetTrack");
    auto get_selected = load<MediaTrack* (*)(ReaProject*, int)>(rec, "GetSelectedTrack");
    auto valid = load<bool (*)(ReaProject*, void*, const char*)>(rec, "ValidatePtr2");
    auto guid = load<GUID* (*)(MediaTrack*)>(rec, "GetTrackGUID");
    auto name = load<bool (*)(MediaTrack*, char*, int)>(rec, "GetTrackName");
    auto get_value = load<double (*)(MediaTrack*, const char*)>(rec, "GetMediaTrackInfo_Value");
    auto set_value = load<bool (*)(MediaTrack*, const char*, double)>(rec, "SetMediaTrackInfo_Value");
    auto version = load<const char* (*)()>(rec, "GetAppVersion");
    auto update = load<void (*)()>(rec, "UpdateArrange");
    auto changes = load<int (*)(ReaProject*)>(rec, "GetProjectStateChangeCount");
    auto begin_undo = load<void (*)(ReaProject*)>(rec, "Undo_BeginBlock2");
    auto end_undo = load<void (*)(ReaProject*, const char*, int)>(rec, "Undo_EndBlock2");
    auto prevent_refresh = load<void (*)(int)>(rec, "PreventUIRefresh");
    Host host;
    host.current_project = [enum_projects] { return enum_projects(-1, nullptr, 0); };
    host.count_tracks = [count_tracks](void* p) { return count_tracks(static_cast<ReaProject*>(p)); };
    host.count_selected_tracks = [count_selected](void* p) { return count_selected(static_cast<ReaProject*>(p)); };
    host.get_track = [get_track](void* p, int i) { return get_track(static_cast<ReaProject*>(p), i); };
    host.get_selected_track = [get_selected](void* p, int i) { return get_selected(static_cast<ReaProject*>(p), i); };
    host.valid_track = [valid](void* p, void* t) { return valid(static_cast<ReaProject*>(p), t, "MediaTrack*"); };
    host.track_guid = [guid](void* t) {
      Guid result{};
      auto source = guid(static_cast<MediaTrack*>(t));
      if (!source) throw Error("STALE_HANDLE", "Track GUID is unavailable");
      static_assert(sizeof(GUID) == sizeof(Guid));
      std::memcpy(result.data(), source, result.size());
      return result;
    };
    host.track_name = [name](void* t) {
      std::vector<char> buffer(65536);
      if (!name(static_cast<MediaTrack*>(t), buffer.data(), static_cast<int>(buffer.size())))
        throw Error("NATIVE_ERROR", "GetTrackName failed");
      return std::string(buffer.data());
    };
    host.get_track_value = [get_value](void* t, const std::string& key) { return get_value(static_cast<MediaTrack*>(t), key.c_str()); };
    host.set_track_value = [set_value](void* t, const std::string& key, double value) {
      return set_value(static_cast<MediaTrack*>(t), key.c_str(), value);
    };
    host.version = [version] { return std::string(version()); };
    host.change_count = [changes](void* project) { return changes(static_cast<ReaProject*>(project)); };
    host.project_generation = [] { return project_generation.load(std::memory_order_relaxed); };
    host.begin_undo = [begin_undo](void* project) { begin_undo(static_cast<ReaProject*>(project)); };
    host.end_undo = [end_undo](void* project, const std::string& label) { end_undo(static_cast<ReaProject*>(project), label.c_str(), -1); };
    host.prevent_refresh = prevent_refresh;
    host.update_arrange = update;
    auto add_dock = load<void (*)(HWND, const char*, const char*, bool)>(rec, "DockWindowAddEx");
    auto remove_dock = load<void (*)(HWND)>(rec, "DockWindowRemove");
    auto dock_index = load<int (*)(HWND, bool*)>(rec, "DockIsChildOfDock");
    auto activate_dock = load<void (*)(HWND)>(rec, "DockWindowActivate");
    auto remember_dock = load<void (*)(const char*, int)>(rec, "Dock_UpdateDockID");
    auto refresh_dock = reinterpret_cast<void (*)(HWND)>(rec->GetFunc("DockWindowRefreshForHWND"));
    DockApi dock{rec->hwnd_main,
      [add_dock](void* h, const std::string& title, const std::string& ident) { add_dock(static_cast<HWND>(h), title.c_str(), ident.c_str(), true); },
      [remove_dock](void* h) { remove_dock(static_cast<HWND>(h)); },
      [dock_index](void* h) { bool floating = false; return dock_index(static_cast<HWND>(h), &floating); },
      [activate_dock](void* h) { activate_dock(static_cast<HWND>(h)); },
      [remember_dock](const std::string& ident, int index) { remember_dock(ident.c_str(), index); },
      [refresh_dock](void* h) { if (refresh_dock) refresh_dock(static_cast<HWND>(h)); }};
    runtime = std::make_unique<Runtime>(std::move(host), fs::u8path(resource()), log_error, std::move(dock));
    add_api("ReaWebOpen", reinterpret_cast<void*>(ReaWebOpen), reinterpret_cast<void*>(open_vararg),
      "int\0const char*\0path\0Open local HTML. Relative paths resolve under resource/Scripts. Returns a window id, or 0 on failure.\0");
    add_api("ReaWeb_Close", reinterpret_cast<void*>(ReaWeb_Close), reinterpret_cast<void*>(id_vararg<ReaWeb_Close>),
      "bool\0int\0windowId\0Close a ReaWebAPI window.\0");
    add_api("ReaWeb_IsOpen", reinterpret_cast<void*>(ReaWeb_IsOpen), reinterpret_cast<void*>(id_vararg<ReaWeb_IsOpen>),
      "bool\0int\0windowId\0Return whether the window is open or initializing.\0");
    add_api("ReaWeb_DevTools", reinterpret_cast<void*>(ReaWeb_DevTools), reinterpret_cast<void*>(id_vararg<ReaWeb_DevTools>),
      "bool\0int\0windowId\0Open Developer Tools (macOS: use Safari's Develop menu).\0");
    add_api("ReaWeb_GetLastError", reinterpret_cast<void*>(ReaWeb_GetLastError), reinterpret_cast<void*>(error_vararg),
      "const char*\0\0\0Return the last runtime error. Async initialization errors are also printed to the REAPER console.\0");
    add_api("ReaWeb_SetDocked", reinterpret_cast<void*>(ReaWeb_SetDocked), reinterpret_cast<void*>(dock_vararg),
      "bool\0int,bool\0windowId,docked\0Dock or float the window. Returns the resulting docked state.\0");
    add_api("ReaWeb_IsDocked", reinterpret_cast<void*>(ReaWeb_IsDocked), reinterpret_cast<void*>(id_vararg<ReaWeb_IsDocked>),
      "bool\0int\0windowId\0Return whether the window belongs to a REAPER Docker.\0");
    add_api("ReaWeb_IsReady", reinterpret_cast<void*>(ReaWeb_IsReady), reinterpret_cast<void*>(id_vararg<ReaWeb_IsReady>),
      "bool\0int\0windowId\0Return whether the current document completed its bridge handshake.\0");
    add_api("ReaWeb_Focus", reinterpret_cast<void*>(ReaWeb_Focus), reinterpret_cast<void*>(id_vararg<ReaWeb_Focus>),
      "bool\0int\0windowId\0Activate the Docker tab or floating window and focus its WebView.\0");
    add_api("ReaWeb_GetDiagnostics", reinterpret_cast<void*>(ReaWeb_GetDiagnostics), reinterpret_cast<void*>(diagnostics_vararg),
      "const char*\0int\0windowId\0Return JSON diagnostics for the window, or an empty object on error.\0");
    add_registration("hwnd_info", reinterpret_cast<void*>(window_info));
    add_registration("projectconfig", &project_events);
    add_registration("timer", reinterpret_cast<void*>(timer));
    return 1;
  } catch (const std::exception& e) { log_error(e.what()); unload(); return 0; }
}
