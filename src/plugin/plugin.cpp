#include <reaper_plugin.h>
#include "runtime/runtime.hpp"
#include "core/file_time.hpp"
#include <cstring>
#include <atomic>
#include <memory>
#include <vector>
#include <cstdio>

namespace {
using namespace reaweb;
std::unique_ptr<Runtime> runtime;
int (*register_api)(const char*, void*) = nullptr;
void (*console)(const char*) = nullptr;
std::string last_error;
std::vector<std::pair<std::string, void*>> registrations;
std::atomic<uint64_t> project_generation{0};
std::atomic<uint64_t> marker_revision{0}, fx_revision{0}, save_revision{0};
std::atomic<uint64_t> selection_revision{0};
std::atomic<uint64_t> transport_revision{0};
class EventSurface final : public IReaperControlSurface {
public:
  const char* GetTypeString() override { return "REAWEBAPI"; }
  const char* GetDescString() override { return "ReaWebAPI event observer"; }
  const char* GetConfigString() override { return ""; }
  void SetSurfaceSelected(MediaTrack*, bool) override { ++selection_revision; }
  void OnTrackSelection(MediaTrack*) override { ++selection_revision; }
  void SetTrackListChange() override { ++selection_revision; }
  void SetPlayState(bool, bool, bool) override { ++transport_revision; }
  void SetSurfaceMute(MediaTrack*, bool) override { ++selection_revision; }
  void SetSurfaceSolo(MediaTrack*, bool) override { ++selection_revision; }
  void SetSurfaceRecArm(MediaTrack*, bool) override { ++selection_revision; }
  int Extended(int call, void*, void*, void*) override {
    // A callback may originate outside the UI thread. Only publish counters;
    // Runtime reads host state and dispatches JavaScript on the main thread.
    if (call == CSURF_EXT_SETPROJECTMARKERCHANGE) { ++marker_revision; return 1; }
    if (call == CSURF_EXT_SETFXCHANGE || call == CSURF_EXT_SETFXPARAM || call == CSURF_EXT_SETFXPARAM_RECFX ||
        call == CSURF_EXT_SETFXENABLED || call == CSURF_EXT_TRACKFX_PRESET_CHANGED || call == CSURF_EXT_TAKEFX_PARAMINFO_CHANGED) {
      ++fx_revision; return 1;
    }
    return 0;
  }
} event_surface;
project_config_extension_t project_events{
  [](const char*, ProjectStateContext*, bool, project_config_extension_t*) { return false; },
  [](ProjectStateContext*, bool is_undo, project_config_extension_t*) { if (!is_undo) ++save_revision; },
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

int ReaWeb_Open(const char* path, const char* instance_key, const char* name, const bool* multiple) {
  return guarded([&] { return runtime->open_instance(path ? path : "", instance_key ? instance_key : "",
    name ? name : "", multiple && *multiple); }, 0);
}
int ReaWeb_OpenDev(const char* url) { return guarded([&] { return runtime->open_dev(url ? url : ""); }, 0); }
bool ReaWeb_Close(int id) { return guarded([&] { return runtime->close(id); }, false); }
bool ReaWeb_IsOpen(int id) { return guarded([&] { return runtime->is_open(id); }, false); }
bool ReaWeb_DevTools(int id) { return guarded([&] { return runtime->devtools(id); }, false); }
bool ReaWeb_SetDocked(int id, bool docked) { return guarded([&] { return runtime->set_docked(id, docked); }, false); }
bool ReaWeb_IsDocked(int id) { return guarded([&] { return runtime->is_docked(id); }, false); }
bool ReaWeb_IsReady(int id) { return guarded([&] { return runtime->is_ready(id); }, false); }
bool ReaWeb_Focus(int id) { return guarded([&] { return runtime->focus(id); }, false); }
bool ReaWeb_Send(int id, const char* message) {
  return guarded([&] {
    if (!message) throw Error("INVALID_ARGUMENT", "Expected a host message string");
    return runtime->send(id, message);
  }, false);
}
const char* ReaWeb_Receive(int id) {
  static std::string value;
  return guarded([&]() -> const char* { value = runtime->receive(id); return value.c_str(); }, "");
}
const char* ReaWeb_GetDiagnostics(int id) {
  static std::string value;
  return guarded([&]() -> const char* { value = runtime->diagnostics(id).dump(); return value.c_str(); }, "{}");
}
const char* ReaWeb_GetLastError() { return last_error.c_str(); }
int ReaWeb_RegisterService(const char* name, const ReaWeb_ServiceCallbacks* callbacks, uint64_t* handle) {
  try { return runtime ? runtime->services().add(name, callbacks, handle) : REAWEB_EXTENSION_UNLOADED; }
  catch (...) { return REAWEB_SERVICE_ERROR; }
}
int ReaWeb_UnregisterService(uint64_t handle) {
  try { return runtime ? runtime->services().remove(handle) : REAWEB_EXTENSION_UNLOADED; }
  catch (...) { return REAWEB_SERVICE_ERROR; }
}
int ReaWeb_CompleteServiceCall(uint64_t handle, uint64_t request, const char* json, int status, const char* message) {
  try { return runtime ? runtime->services().complete(handle, request, json, status, message) : REAWEB_EXTENSION_UNLOADED; }
  catch (...) { return REAWEB_SERVICE_ERROR; }
}
int ReaWeb_EmitServiceEvent(uint64_t handle, int window, const char* name, const char* json) {
  try { return runtime ? runtime->services().emit(handle, window, name, json) : REAWEB_EXTENSION_UNLOADED; }
  catch (...) { return REAWEB_SERVICE_ERROR; }
}
void* open_vararg(void** args, int count) {
  return reinterpret_cast<void*>(static_cast<intptr_t>(ReaWeb_Open(
    count >= 1 ? static_cast<const char*>(args[0]) : nullptr,
    count >= 2 ? static_cast<const char*>(args[1]) : nullptr,
    count >= 3 ? static_cast<const char*>(args[2]) : nullptr,
    count >= 4 ? static_cast<const bool*>(args[3]) : nullptr)));
}
void* dev_vararg(void** args, int count) {
  return reinterpret_cast<void*>(static_cast<intptr_t>(ReaWeb_OpenDev(count >= 1 ? static_cast<const char*>(args[0]) : nullptr)));
}
template<bool (*Fn)(int)> void* id_vararg(void** args, int count) {
  const auto id = count >= 1 ? static_cast<int>(reinterpret_cast<intptr_t>(args[0])) : 0;
  return reinterpret_cast<void*>(static_cast<intptr_t>(Fn(id)));
}
void* error_vararg(void**, int) { return const_cast<char*>(ReaWeb_GetLastError()); }
void* send_vararg(void** args, int count) {
  return reinterpret_cast<void*>(static_cast<intptr_t>(ReaWeb_Send(
    count >= 1 ? static_cast<int>(reinterpret_cast<intptr_t>(args[0])) : 0,
    count >= 2 ? static_cast<const char*>(args[1]) : nullptr)));
}
void* receive_vararg(void** args, int count) {
  return const_cast<char*>(ReaWeb_Receive(count >= 1 ? static_cast<int>(reinterpret_cast<intptr_t>(args[0])) : 0));
}
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
    auto count_selected = load<int (*)(ReaProject*)>(rec, "CountSelectedTracks");
    auto get_selected = load<MediaTrack* (*)(ReaProject*, int)>(rec, "GetSelectedTrack");
    auto valid = load<bool (*)(ReaProject*, void*, const char*)>(rec, "ValidatePtr2");
    auto guid = load<GUID* (*)(MediaTrack*)>(rec, "GetTrackGUID");
    auto update = load<void (*)()>(rec, "UpdateArrange");
    auto changes = load<int (*)(ReaProject*)>(rec, "GetProjectStateChangeCount");
    auto begin_undo = load<void (*)(ReaProject*)>(rec, "Undo_BeginBlock2");
    auto end_undo = load<void (*)(ReaProject*, const char*, int)>(rec, "Undo_EndBlock2");
    auto prevent_refresh = load<void (*)(int)>(rec, "PreventUIRefresh");
    Host host;
    host.native_function = rec->GetFunc;
    host.valid_window = [](void* w) { return IsWindow(static_cast<HWND>(w)) != 0; };
    host.current_project = [enum_projects] { return enum_projects(-1, nullptr, 0); };
    host.count_selected_tracks = [count_selected](void* p) { return count_selected(static_cast<ReaProject*>(p)); };
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
    host.change_count = [changes](void* project) { return changes(static_cast<ReaProject*>(project)); };
    host.project_generation = [] { return project_generation.load(std::memory_order_relaxed); };
    host.begin_undo = [begin_undo](void* project) { begin_undo(static_cast<ReaProject*>(project)); };
    host.end_undo = [end_undo](void* project, const std::string& label) { end_undo(static_cast<ReaProject*>(project), label.c_str(), -1); };
    host.prevent_refresh = prevent_refresh;
    host.update_arrange = update;
    const auto get_function = rec->GetFunc;
    auto count_tracks = reinterpret_cast<int (*)(ReaProject*)>(get_function("CountTracks"));
    auto get_track = reinterpret_cast<MediaTrack* (*)(ReaProject*, int)>(get_function("GetTrack"));
    if (count_tracks && get_track) {
      host.track_count = [count_tracks, enum_projects] { return count_tracks(enum_projects(-1, nullptr, 0)); };
      host.track_identity = [get_track, enum_projects, guid](int index) {
        auto track = get_track(enum_projects(-1, nullptr, 0), index);
        auto value = track ? guid(track) : nullptr;
        if (!value) return std::string();
        char text[40];
        std::snprintf(text, sizeof(text), "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
          static_cast<unsigned>(value->Data1), value->Data2, value->Data3, value->Data4[0], value->Data4[1],
          value->Data4[2], value->Data4[3], value->Data4[4], value->Data4[5], value->Data4[6], value->Data4[7]);
        return std::string(text);
      };
    }
    host.event_revision = [](const std::string& name) {
      if (name == "transport") return transport_revision.load();
      if (name == "track-selected") return selection_revision.load();
      if (name == "marker-changed") return marker_revision.load();
      if (name == "fx-changed") return fx_revision.load();
      return uint64_t{0};
    };
    auto dirty = reinterpret_cast<int (*)(ReaProject*)>(get_function("IsProjectDirty"));
    if (dirty) host.project_save_state = [enum_projects, dirty] {
      char name[32768]{}; auto project = enum_projects(-1, name, sizeof(name));
      std::error_code error;
      auto stamp = fs::last_write_time(fs::u8path(name), error);
      return Json{{"available", !error && name[0] != 0}, {"path", name}, {"dirty", dirty(project) != 0},
        {"stamp", error ? "" : file_time_ticks(stamp.time_since_epoch().count())}, {"serialization", save_revision.load()}};
    };
    auto count_items = reinterpret_cast<int (*)(ReaProject*)>(get_function("CountSelectedMediaItems"));
    auto selected_item = reinterpret_cast<MediaItem* (*)(ReaProject*, int)>(get_function("GetSelectedMediaItem"));
    auto active_take = reinterpret_cast<MediaItem_Take* (*)(MediaItem*)>(get_function("GetActiveTake"));
    auto item_string = reinterpret_cast<bool (*)(MediaItem*, const char*, char*, bool)>(get_function("GetSetMediaItemInfo_String"));
    auto take_string = reinterpret_cast<bool (*)(MediaItem_Take*, const char*, char*, bool)>(get_function("GetSetMediaItemTakeInfo_String"));
    if (count_items && selected_item && active_take && item_string && take_string) {
      host.count_selected_items = [count_items](void* p) { return count_items(static_cast<ReaProject*>(p)); };
      host.item_identity = [=](void* p, int index) -> std::pair<std::string, std::string> {
        auto item = selected_item(static_cast<ReaProject*>(p), index);
        if (!item || !valid(static_cast<ReaProject*>(p), item, "MediaItem*")) return {};
        char item_guid[128]{}, take_guid[128]{};
        if (!item_string(item, "GUID", item_guid, false)) return {};
        if (auto take = active_take(item)) take_string(take, "GUID", take_guid, false);
        return {item_guid, take_guid};
      };
    }
    host.event_snapshot = [get_function](const std::string& name) -> Json {
      if (name == "transportchange") {
        auto state = reinterpret_cast<int (*)()>(get_function("GetPlayState"));
        auto position = reinterpret_cast<double (*)()>(get_function("GetPlayPosition"));
        auto cursor = reinterpret_cast<double (*)()>(get_function("GetCursorPosition"));
        auto tempo = reinterpret_cast<double (*)()>(get_function("Master_GetTempo"));
        if (!state || !position || !cursor || !tempo) return {{"available", false}};
        return {{"available", true}, {"state", state()}, {"position", position()}, {"cursor", cursor()}, {"tempo", tempo()}};
      }
      auto focused = reinterpret_cast<bool (*)(int,int*,int*,int*,int*,int*)>(get_function("GetTouchedOrFocusedFX"));
      if (!focused) return {{"available", false}, {"focused", nullptr}, {"touched", nullptr}};
      auto snapshot = [&](int mode) -> Json {
        int track = -1, item = -1, take = -1, fx = -1, parameter = -1;
        if (!focused(mode, &track, &item, &take, &fx, &parameter)) return nullptr;
        Json value = nullptr;
        if (mode == 0 && parameter >= 0) {
          auto get_track = reinterpret_cast<MediaTrack* (*)(ReaProject*, int)>(get_function("GetTrack"));
          auto master = reinterpret_cast<MediaTrack* (*)(ReaProject*)>(get_function("GetMasterTrack"));
          auto t = track < 0 ? (master ? master(nullptr) : nullptr) : (get_track ? get_track(nullptr, track) : nullptr);
          if (t && item < 0) {
            auto get = reinterpret_cast<double (*)(MediaTrack*,int,int)>(get_function("TrackFX_GetParamNormalized"));
            if (get) value = get(t, fx, parameter);
          } else if (t) {
            auto get_item = reinterpret_cast<MediaItem* (*)(MediaTrack*,int)>(get_function("GetTrackMediaItem"));
            auto get_take = reinterpret_cast<MediaItem_Take* (*)(MediaItem*,int)>(get_function("GetTake"));
            auto get = reinterpret_cast<double (*)(MediaItem_Take*,int,int)>(get_function("TakeFX_GetParamNormalized"));
            auto media = get_item ? get_item(t, item) : nullptr;
            auto source = media && get_take ? get_take(media, take) : nullptr;
            if (source && get) value = get(source, fx, parameter);
          }
        }
        return {{"trackIndex", track}, {"itemIndex", item}, {"takeIndex", take}, {"fxIndex", fx},
          {"parameter", mode == 0 ? Json(parameter) : Json(nullptr)}, {"focused", mode == 1 ? Json(!(parameter & 1)) : Json(nullptr)}, {"value", value}};
      };
      return {{"available", true}, {"focused", snapshot(1)}, {"touched", snapshot(0)}};
    };
    auto add_dock = load<void (*)(HWND, const char*, const char*, bool)>(rec, "DockWindowAddEx");
    auto remove_dock = load<void (*)(HWND)>(rec, "DockWindowRemove");
    auto dock_index = load<int (*)(HWND, bool*)>(rec, "DockIsChildOfDock");
    auto activate_dock = load<void (*)(HWND)>(rec, "DockWindowActivate");
    auto remember_dock = load<void (*)(const char*, int)>(rec, "Dock_UpdateDockID");
    auto refresh_dock = reinterpret_cast<void (*)(HWND)>(rec->GetFunc("DockWindowRefreshForHWND"));
    DockApi dock{rec->hwnd_main,
      // A nonempty Docker label is fixed. Let REAPER read the current HWND title on refresh.
      [add_dock](void* h, const std::string&, const std::string& ident) { add_dock(static_cast<HWND>(h), nullptr, ident.c_str(), true); },
      [remove_dock](void* h) { remove_dock(static_cast<HWND>(h)); },
      [dock_index](void* h) { bool floating = false; return dock_index(static_cast<HWND>(h), &floating); },
      [activate_dock](void* h) { activate_dock(static_cast<HWND>(h)); },
      [remember_dock](const std::string& ident, int index) { remember_dock(ident.c_str(), index); },
      [refresh_dock, dock_index, activate_dock](void* h) {
        auto hwnd = static_cast<HWND>(h);
        if (refresh_dock) refresh_dock(hwnd);
        // Refresh only the selected, visible tab so REAPER also updates its floating Docker caption.
        bool floating = false;
        if (dock_index(hwnd, &floating) >= 0 && floating && IsWindowVisible(hwnd)) activate_dock(hwnd);
      }};
    runtime = std::make_unique<Runtime>(std::move(host), fs::u8path(resource()), log_error, std::move(dock));
    add_registration("API_ReaWeb_RegisterService", reinterpret_cast<void*>(ReaWeb_RegisterService));
    add_registration("API_ReaWeb_UnregisterService", reinterpret_cast<void*>(ReaWeb_UnregisterService));
    add_registration("API_ReaWeb_CompleteServiceCall", reinterpret_cast<void*>(ReaWeb_CompleteServiceCall));
    add_registration("API_ReaWeb_EmitServiceEvent", reinterpret_cast<void*>(ReaWeb_EmitServiceEvent));
    add_api("ReaWeb_Open", reinterpret_cast<void*>(ReaWeb_Open), reinterpret_cast<void*>(open_vararg),
      "int\0const char*,const char*,const char*,const bool*\0path,instanceKeyInOptional,idInOptional,multipleInOptional\0Open local HTML. Relative paths resolve under resource/Scripts. An instanceKey reuses and focuses its window, optionally scoped by id. Omit the key or set multiple=true to create a new window. Returns a window id, or 0 on failure.\0");
    add_api("ReaWeb_Close", reinterpret_cast<void*>(ReaWeb_Close), reinterpret_cast<void*>(id_vararg<ReaWeb_Close>),
      "bool\0int\0windowId\0Close a ReaWebAPI window.\0");
    add_api("ReaWeb_IsOpen", reinterpret_cast<void*>(ReaWeb_IsOpen), reinterpret_cast<void*>(id_vararg<ReaWeb_IsOpen>),
      "bool\0int\0windowId\0Return whether the window is open or initializing.\0");
    add_api("ReaWeb_DevTools", reinterpret_cast<void*>(ReaWeb_DevTools), reinterpret_cast<void*>(id_vararg<ReaWeb_DevTools>),
      "bool\0int\0windowId\0Request showing DevTools. Returns false when native Inspector controls are unavailable.\0");
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
    add_api("ReaWeb_Send", reinterpret_cast<void*>(ReaWeb_Send), reinterpret_cast<void*>(send_vararg),
      "bool\0int,const char*\0windowId,message\0Queue UTF-8 text for the ready document's message event. Returns true when accepted. Limit 1 MiB, no NUL.\0");
    add_api("ReaWeb_Receive", reinterpret_cast<void*>(ReaWeb_Receive), reinterpret_cast<void*>(receive_vararg),
      "const char*\0int\0windowId\0Pop the next WebView message without blocking. Empty queue or error returns an empty string.\0");
    add_api("ReaWeb_OpenDev", reinterpret_cast<void*>(ReaWeb_OpenDev), reinterpret_cast<void*>(dev_vararg),
      "int\0const char*\0url\0Open an explicitly trusted loopback HTTP development server.\0");
    add_registration("hwnd_info", reinterpret_cast<void*>(window_info));
    add_registration("projectconfig", &project_events);
    add_registration("csurf_inst", &event_surface);
    add_registration("timer", reinterpret_cast<void*>(timer));
    return 1;
  } catch (const std::exception& e) { log_error(e.what()); unload(); return 0; }
}
