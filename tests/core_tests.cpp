#include "core/core.hpp"
#include "host_adapter.hpp"
#include <fstream>
#include <iostream>

using namespace reaweb;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("Check failed: " #condition); } while (false)
struct Fixture {
  int project_storage = 0, other_project = 0, track_storage = 0;
  void* current = &project_storage;
  bool valid = true, selected = true, closed = false, inspected = false, docked = false;
  Guid guid{};
  double volume = 1, cursor = 0;
  int color = 0, cursor_writes = 0, color_writes = 0;
  adapter::Host host;
  std::unique_ptr<Bridge> bridge;
  Fixture() {
    host.current_project = [&] { return current; };
    host.count_tracks = [](void*) { return 3; };
    host.count_selected_tracks = [&](void*) { return selected ? 1 : 0; };
    host.get_track = [&](void*, int index) -> void* { return index < 3 ? &track_storage : nullptr; };
    host.get_selected_track = [&](void*, int index) -> void* { return selected && index == 0 ? &track_storage : nullptr; };
    host.valid_track = [&](void* p, void* t) { return valid && p == current && t == &track_storage; };
    host.track_guid = [&](void*) { return guid; };
    host.track_name = [](void*) { return std::string(u8"Guitar 吉他 \"A\"\nLine\u2028End"); };
    host.get_track_value = [&](void*, const std::string& key) { return key == "I_CUSTOMCOLOR" ? color : volume; };
    host.set_track_value = [&](void*, const std::string& key, double value) { if (key == "I_CUSTOMCOLOR") color = static_cast<int>(value); else volume = value; return true; };
    host.version = [] { return "7.test"; };
    host.project_name = [](void*) { return std::string(u8"Demo 工程.rpp"); };
    host.change_count = [](void*) { return 3; };
    host.cursor_position = [&] { return cursor; };
    host.play_position = [] { return 1.25; };
    host.play_state = [] { return 1; };
    host.tempo = [] { return 120.0; };
    host.set_cursor = [&](double t, bool, bool) { cursor = t; ++cursor_writes; };
    host.time_to_beats = [](void*, double t) { return Json::array({t * 2, 0, 4, t * 2, 4}); };
    host.count_markers = [](void*) { return Json::array({2, 1, 1}); };
    host.enum_marker = [](void*, int i) { return Json::array({i < 2 ? 1 : 0, i == 1, 2.5, 5.0, u8"段落", i + 1, 0}); };
    host.track_color = [&](void*) { return color; };
    host.set_track_color = [&](void*, int value) { color = (value & 0xffffff) | 0x1000000; ++color_writes; };
    host.color_to_native = [](int r, int g, int b) { return r | (g << 8) | (b << 16); };
    host.color_from_native = [](int c) { return Json::array({c & 255, (c >> 8) & 255, (c >> 16) & 255}); };
    host.track_fx_count = [](void*) { return 1; };
    host.track_fx_name = [](void*, int i) { return Json::array({i == 0, i == 0 ? "EQ" : ""}); };
    host.track_fx_num_params = [](void*, int i) { return i == 0 ? 1 : 0; };
    host.track_fx_param = [](void*, int, int) { return Json::array({0.5, 0.0, 1.0}); };
    adapter::connect(host);
    bridge = std::make_unique<Bridge>(host, Bridge::Controls{
      [](const std::string& path) { CHECK(path == "Other/index.html"); return 7; },
      [&] { closed = true; }, [&] { inspected = true; },
      [&](bool value) { return docked = value; }, [&] { return docked; }
    }, "session-one");
  }
  Json call(const char* method, std::vector<Json> args = {}) {
    adapter::host=&host;
    return bridge->dispatch(Json{{"id", 42}, {"document", "page-one"}, {"method", method}, {"args", args}}.dump());
  }
};
int main() {
  try {
    Fixture f;
    auto coverage = f.call("ReaWeb_GetCapabilities")["result"]["api"];
    CHECK(coverage["official"] == 730);
    CHECK(coverage["implemented"] == 730);
    CHECK(coverage["compatible"] == 730);
    CHECK(coverage["partial"] == 0);
    CHECK(coverage["missing"] == 0);
    CHECK(coverage["bindings"]["GetTrackName"]["compatibility"] == "compatible");
    CHECK(f.call("InsertTrackAtIndex", {0, false})["error"]["code"] == "API_UNAVAILABLE");
    CHECK(f.call("NotAReaperAPI")["error"]["code"] == "UNKNOWN_API");
    CHECK(f.call("CountTracks", {0})["result"] == 3);
    CHECK(f.call("GetSelectedTrack", {0, 0})["result"].is_object());
    auto handle = f.call("GetSelectedTrack", {0, 0})["result"];
    CHECK(handle == f.call("GetSelectedTrack", {0, 0})["result"]);
    CHECK(f.call("GetProjectName", {0})["result"] == u8"Demo 工程.rpp");
    CHECK(f.call("GetProjectName")["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.call("GetProjectName", {1})["error"]["code"] == "INVALID_HANDLE");
    CHECK(f.call("GetProjectStateChangeCount", {0})["result"] == 3);
    CHECK(f.call("SetEditCurPos", {-0.25, true, false})["result"].is_null());
    CHECK(f.call("GetCursorPosition")["result"] == -0.25);
    CHECK(f.call("SetEditCurPos", {1.0, 1, false})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.call("SetEditCurPos", {"1", true, false})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.cursor_writes == 1);
    CHECK(f.call("TimeMap2_timeToBeats", {0, 1.25})["result"] == Json::array({2.5, 0, 4, 2.5, 4}));
    CHECK(f.call("CountProjectMarkers", {0})["result"] == Json::array({2, 1, 1}));
    CHECK(f.call("EnumProjectMarkers3", {0, 1})["result"][1] == true);
    CHECK(f.call("GetPlayPosition")["result"] == 1.25);
    CHECK(f.call("GetPlayState")["result"] == 1);
    CHECK(f.call("Master_GetTempo")["result"] == 120);
    const auto native_color = f.call("ColorToNative", {37, 149, 211})["result"];
    CHECK(f.call("ColorFromNative", {native_color})["result"] == Json::array({37, 149, 211}));
    CHECK(f.call("ColorFromNative", {4294967296ULL})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.call("SetTrackColor", {handle, native_color})["result"].is_null());
    CHECK(f.call("GetTrackColor", {handle})["result"] == (native_color.get<int>() | 0x1000000));
    CHECK(f.call("SetTrackColor", {handle, 1.5})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.color_writes == 1);
    CHECK(f.call("SetTrackColor", {handle, 0})["result"].is_null());
    CHECK(f.call("GetTrackColor", {handle})["result"] == 0x1000000);
    CHECK(f.call("SetMediaTrackInfo_Value", {handle, "I_CUSTOMCOLOR", 0})["result"] == true);
    CHECK(f.call("GetTrackColor", {handle})["result"] == 0);
    CHECK(f.call("TrackFX_GetCount", {handle})["result"] == 1);
    CHECK(f.call("TrackFX_GetFXName", {handle, 0})["result"] == Json::array({true, "EQ"}));
    CHECK(f.call("TrackFX_GetNumParams", {handle, 0})["result"] == 1);
    CHECK(f.call("TrackFX_GetParam", {handle, 0, 0})["result"] == Json::array({0.5, 0.0, 1.0}));
    {
      Fixture color_batch;
      int undo_begin = 0, undo_end = 0, refresh_depth = 0;
      color_batch.host.begin_undo = [&](void*) { ++undo_begin; };
      color_batch.host.end_undo = [&](void*, const std::string&) { ++undo_end; };
      color_batch.host.prevent_refresh = [&](int value) { refresh_depth += value; };
      auto h = color_batch.call("GetSelectedTrack", {0, 0})["result"];
      Json calls = Json::array({Json{{"method", "SetTrackColor"}, {"args", {h, 0}}}});
      auto result = color_batch.call("ReaWeb_Batch", {calls, {{"undoLabel", "Color test"}}});
      CHECK(result["result"] == Json::array({nullptr}));
      CHECK(color_batch.color == 0x1000000 && undo_begin == 1 && undo_end == 1 && refresh_depth == 0);
      calls.push_back(Json{{"method", "SetMediaTrackInfo_Value"}, {"args", {h, "I_CUSTOMCOLOR", 33554432}}});
      CHECK(color_batch.call("ReaWeb_Batch", {calls, {{"undoLabel", "Invalid color"}}})["error"]["code"] == "INVALID_ARGUMENT");
      CHECK(color_batch.color_writes == 1 && undo_begin == 1 && undo_end == 1 && refresh_depth == 0);
    }
    {
      Fixture queries;
      Json calls = Json::array({
        {{"method", "GetPlayPosition"}, {"args", Json::array()}},
        {{"method", "GetPlayState"}, {"args", Json::array()}},
        {{"method", "Master_GetTempo"}, {"args", Json::array()}},
        {{"method", "TimeMap2_timeToBeats"}, {"args", {0, Json{{"$ref", 0}}}}},
        {{"method", "GetProjectName"}, {"args", {0}}},
        {{"method", "ColorToNative"}, {"args", {37, 149, 211}}},
        {{"method", "ColorFromNative"}, {"args", {Json{{"$ref", 5}}}}}
      });
      CHECK(queries.call("ReaWeb_Batch", {calls})["result"] ==
        Json::array({1.25, 1, 120, Json::array({2.5, 0, 4, 2.5, 4}), u8"Demo 工程.rpp",
          37 | (149 << 8) | (211 << 16), Json::array({37, 149, 211})}));
      queries.host.play_position = [] { return 0.0; };
      queries.host.play_state = [] { return 0; };
      auto stopped = queries.call("ReaWeb_Batch", {calls})["result"];
      CHECK(stopped[0] == 0 && stopped[1] == 0);
      queries.bridge->validate_managed_call("GetPlayPosition", Json::array());
      calls[0]["args"] = {1};
      CHECK(queries.call("ReaWeb_Batch", {calls})["error"]["code"] == "INVALID_ARGUMENT");
      CHECK(queries.call("ReaWeb_Batch", {Json::array({
        {{"method", "GetPlayPositionEx"}, {"args", {0}}}
      })})["error"]["code"] == "API_UNAVAILABLE");

      auto track = queries.call("GetTrack", {0, 0})["result"];
      Json probes = Json::array({
        {{"method", "ValidatePtr"}, {"args", {track, "MediaTrack*"}}},
        {{"method", "ValidatePtr2"}, {"args", {0, track, "MediaTrack*"}}},
        {{"method", "ValidatePtr2"}, {"args", {0, nullptr, "MediaTrack*"}}}
      });
      CHECK(queries.call("ReaWeb_Batch", {probes})["result"] == Json::array({true, true, false}));
      Json dependent_probes = Json::array({
        {{"method", "GetSelectedTrack"}, {"args", {0, 99}}},
        {{"method", "ValidatePtr2"}, {"args", {0, Json{{"$ref", 0}}, "MediaTrack*"}}}
      });
      CHECK(queries.call("ReaWeb_Batch", {dependent_probes})["result"] == Json::array({nullptr, false}));
      dependent_probes.push_back({{"method", "GetTrackName"}, {"args", {Json{{"$ref", 0}}}}});
      auto missing_track = queries.call("ReaWeb_Batch", {dependent_probes});
      CHECK(missing_track["error"]["code"] == "BATCH_FAILED");
      CHECK(missing_track["error"]["details"]["completed"] == 2);
      CHECK(missing_track["error"]["details"]["results"] == Json::array({nullptr, false}));
      queries.valid = false;
      CHECK(queries.call("ReaWeb_Batch", {probes})["result"] == Json::array({false, false, false}));
      CHECK(queries.call("GetTrackName", {track})["error"]["code"] == "STALE_HANDLE");
      CHECK(queries.call("ReaWeb_Batch", {probes})["result"] == Json::array({false, false, false}));
      // A malformed token must not be treated as a native null pointer.
      probes[0]["args"][0]["id"] = 42;
      CHECK(queries.call("ReaWeb_Batch", {probes})["error"]["code"] == "INVALID_HANDLE");
    }
    auto name = f.call("GetTrackName", {handle});
    if (name["result"][1] != f.host.track_name(nullptr)) std::cerr << "GetTrackName response: " << name.dump() << '\n';
    CHECK(name["result"][1] == f.host.track_name(nullptr));
    CHECK(name["document"] == "page-one");
    CHECK(name.dump(-1, ' ', true).find("\\u2028") != std::string::npos);
    CHECK(f.call("SetMediaTrackInfo_Value", {handle, "D_VOL", 0.5})["result"] == true);
    CHECK(f.volume == 0.5);
    CHECK(f.call("CountTracks", {1})["error"]["code"] == "INVALID_HANDLE");
    CHECK(f.call("GetTrack", {0, 0.5})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.call("GetTrack", {0, 18446744073709551615ull})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.call("GetTrack", {0, 99})["result"].is_null());
    CHECK(f.call("executeNative")["error"]["code"] == "UNKNOWN_API");
    CHECK(f.call("GetTrackName")["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.bridge->dispatch("{bad")["error"]["code"] == "INVALID_REQUEST");
    CHECK(f.bridge->dispatch(std::string(64*1024*1024+1, 'x'))["error"]["code"] == "MESSAGE_LIMIT");
    CHECK(f.bridge->dispatch(std::string(1000, '[') + std::string(1000, ']'))["error"]["code"] == "INVALID_REQUEST");
    CHECK(f.bridge->dispatch("{\"id\":9007199254740992}")["error"]["code"] == "INVALID_REQUEST");
    f.selected = false;
    CHECK(f.call("GetSelectedTrack", {0, 0})["result"].is_null());
    f.valid = false;
    CHECK(f.call("GetTrackName", {handle})["error"]["code"] == "STALE_HANDLE");
    f.valid = true; f.selected = true;
    handle = f.call("GetSelectedTrack", {0, 0})["result"];
    f.guid[0] = 1;
    CHECK(f.call("GetTrackName", {handle})["error"]["code"] == "STALE_HANDLE");
    handle = f.call("GetSelectedTrack", {0, 0})["result"];
    f.current = &f.other_project;
    CHECK(f.call("GetTrackName", {handle})["error"]["code"] == "STALE_HANDLE");
    Fixture other;
    auto foreign = other.call("GetSelectedTrack", {0, 0})["result"];
    foreign["id"] = "other-window:1";
    CHECK(f.call("GetTrackName", {foreign})["error"]["code"] == "STALE_HANDLE");
    handle = f.call("GetSelectedTrack", {0, 0})["result"];
    int began = 0, ended = 0, refresh = 0, updates = 0, writes = 0;
    f.host.begin_undo = [&](void*) { ++began; };
    f.host.end_undo = [&](void*, const std::string& label) { CHECK(label == "Mixer"); ++ended; };
    f.host.prevent_refresh = [&](int value) { refresh += value; CHECK(refresh >= 0); };
    f.host.update_arrange = [&] { ++updates; };
    f.host.set_track_value = [&](void*, const std::string&, double value) { ++writes; f.volume = value; return true; };
    Json write = {{"method", "SetMediaTrackInfo_Value"}, {"args", {handle, "D_VOL", 0.25}}};
    Json read = {{"method", "GetMediaTrackInfo_Value"}, {"args", {handle, "D_VOL"}}};
    auto batch = f.call("ReaWeb_Batch", {Json::array({write, read}), {{"undoLabel", "Mixer"}}});
    CHECK(batch["result"] == Json::array({true, 0.25}));
    CHECK(began == 1 && ended == 1 && refresh == 0 && writes == 1 && updates == 1);
    auto bad = write; bad["args"][2] = -1;
    CHECK(f.call("ReaWeb_Batch", {Json::array({write, bad}), {{"undoLabel", "Mixer"}}})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(writes == 1 && began == 1);
    f.host.set_track_value = [&](void*, const std::string&, double) { return ++writes != 3; };
    batch = f.call("ReaWeb_Batch", {Json::array({write, write}), {{"undoLabel", "Mixer"}}});
    CHECK(batch["error"]["code"] == "BATCH_FAILED");
    CHECK(batch["error"]["details"]["completed"] == 1 && batch["error"]["details"]["rolledBack"] == false);
    CHECK(ended == 2 && began == 2 && refresh == 0 && updates == 2);
    CHECK(f.call("ReaWeb_Batch", {Json::array({{{"method", "ReaWeb_Close"}, {"args", Json::array()}}})})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.call("ReaWeb_Open", {"Other/index.html"})["result"] == 7);
    CHECK(f.call("ReaWeb_Open", {42})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.call("ReaWebOpen", {"Other/index.html"})["error"]["code"] == "UNKNOWN_API");
    f.call("ReaWeb_DevTools"); f.call("ReaWeb_Close");
    CHECK(f.inspected && f.closed);
    CHECK(f.call("ReaWeb_SetDocked", {true})["result"] == true);
    CHECK(f.call("ReaWeb_IsDocked")["result"] == true);
    CHECK(f.call("ReaWeb_SetDocked", {false})["result"] == false);
    CHECK(f.call("ReaWeb_SetDocked", {1})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.call("ReaWeb_GetCapabilities")["result"]["methods"].size() == 781);
    CHECK(f.call("ReaWeb_GetCapabilities")["result"]["runtime"]["contract"] == 2);
    CHECK(f.call("ReaWeb_GetCapabilities")["result"]["runtime"]["namespaces"] ==
      Json({"window", "theme", "dialog", "events", "lifecycle", "debug", "fs", "audio",
            "clipboard", "dragDrop", "app", "system", "transaction", "host"}));
    CHECK(f.call("ReaWeb_GetCapabilities")["result"]["runtime"]["reservedNamespaces"] == Json::array());
    CHECK(f.call("ReaWeb_GetCapabilities")["result"]["version"] == REAWEB_VERSION);
    CHECK(same_document("file:///a/index.html#x", "file:///a/index.html"));
    CHECK(!same_document("file:///a/evil.html", "file:///a/index.html"));
    CHECK(!same_document("https://evil.example/", "file:///a/index.html"));
    auto root = fs::current_path() / "path-test-fixture";
    fs::create_directories(root);
    const auto file = root / fs::u8path(u8"空 格#%.html");
    std::ofstream(file) << "<html></html>";
    CHECK(resolve_html(root, file.filename().u8string()) == fs::canonical(file));
    auto uri = file_uri(file);
    CHECK(uri.find("%20") != std::string::npos && uri.find("%23%25.html") != std::string::npos);
    for (const auto& bad : {"missing.html", "https://example.com/index.html", "file:///tmp/test.html"}) {
      bool rejected = false;
      try { resolve_html(root, bad); } catch (const Error&) { rejected = true; }
      CHECK(rejected);
    }
    fs::remove(file); fs::remove(root);
    std::cout << "Core: API dispatch, validation, stale handles, project switches, limits, Unicode and paths passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
