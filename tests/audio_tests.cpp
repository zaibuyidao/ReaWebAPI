#include "runtime/services.hpp"
#include <reaper_plugin.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
using namespace reaweb;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("Check failed: " #x); } while (false)
namespace {
const auto main_thread = std::this_thread::get_id();
int alive = 0, builds = 0, finishes = 0, steps = 0, peak_mode = 0;
int project, track;
bool valid_track = true;
bool selected = true;
int track_index = 0, other_track, writes = 0, cleared = 0;
char** registered_path;
int* registered_size;
GUID track_guid{};
void on_main() { CHECK(std::this_thread::get_id() == main_thread); }
class Source final : public PCM_source {
public:
  Source() { on_main(); ++alive; }
  ~Source() override { on_main(); --alive; }
  PCM_source* Duplicate() override { return new Source; }
  bool IsAvailable() override { on_main(); return true; }
  const char* GetType() override { return "WAV"; }
  bool SetFileName(const char*) override { return false; }
  int GetNumChannels() override { return 2; }
  double GetSampleRate() override { return 48000; }
  double GetLength() override { return 2; }
  int GetBitsPerSample() override { return 24; }
  int PropertiesWindow(HWND) override { return 0; }
  void GetSamples(PCM_source_transfer_t*) override {}
  void GetPeakInfo(PCM_source_peaktransfer_t*) override {}
  void SaveState(ProjectStateContext*) override {}
  int LoadState(const char*, ProjectStateContext*) override { return 0; }
  void Peaks_Clear(bool) override {}
  int PeaksBuild_Begin() override { return 0; }
  int PeaksBuild_Run() override { return 0; }
  void PeaksBuild_Finish() override {}
};
void* resolve(const char* name) {
  if (!strcmp(name, "EnumProjects")) return reinterpret_cast<void*>(+[](int, char* buffer, int) -> void* { if (buffer) buffer[0] = 0; return &project; });
  if (!strcmp(name, "GetTrack")) return reinterpret_cast<void*>(+[](void*, int i) -> void* { return i == track_index ? &track : &other_track; });
  if (!strcmp(name, "SetMediaTrackInfo_Value")) return reinterpret_cast<void*>(+[](void* t, const char* key, double v) {
    CHECK(t == &track && !strcmp(key, "D_VOL") && v == .5); ++writes; return true;
  });
  if (!strcmp(name, "realloc_cmd_register_buf")) return reinterpret_cast<void*>(+[](char** p, int* size) {
    registered_path = p; registered_size = size; return 17;
  });
  if (!strcmp(name, "realloc_cmd_clear")) return reinterpret_cast<void*>(+[](int token) {
    CHECK(token == 17); ++cleared; registered_path = nullptr; registered_size = nullptr;
  });
  if (!strcmp(name, "GetUserFileName")) return reinterpret_cast<void*>(+[](int mode, const char* title, const char* initial,
      const char* filters, char* path, int size) {
    CHECK(path == *registered_path && size == *registered_size);
    CHECK(!strcmp(title, "Title") && !strcmp(initial, u8"选择") && !strcmp(filters, "Audio|*.wav"));
    if (mode == 0) return false;
    static char result[] = u8"/选择/音频";
    *registered_path = result; *registered_size = sizeof(result); return true;
  });
  if (!strcmp(name, "GetTrackGUID")) return reinterpret_cast<void*>(+[](void*) -> GUID* { return &track_guid; });
  if (!strcmp(name, "GetTrackName")) return reinterpret_cast<void*>(+[](void* value, char* buf, int size) {
    CHECK(value == &track && size >= 6); std::memcpy(buf, "Track", 6); return true;
  });
  if (!strcmp(name, "ValidatePtr2")) return reinterpret_cast<void*>(+[](void*, void* value, const char*) { return value == &project || (valid_track && value == &track); });
  if (!strcmp(name, "GetMediaTrackInfo_Value")) return reinterpret_cast<void*>(+[](void* value, const char* key) {
    on_main(); CHECK(value == &track); if (!strcmp(key, "D_VOL")) return .5;
    CHECK(!strcmp(key, "I_NCHAN")); return 2.0;
  });
  if (!strcmp(name, "Track_GetPeakInfo")) return reinterpret_cast<void*>(+[](void* value, int channel) { on_main(); CHECK(value == &track); return channel ? 0.0 : 0.5; });
  if (!strcmp(name, "PCM_Source_CreateFromFileEx")) return reinterpret_cast<void*>(+[](const char*, bool force_audio) -> PCM_source* { CHECK(force_audio); return new Source; });
  if (!strcmp(name, "PCM_Source_Destroy")) return reinterpret_cast<void*>(+[](PCM_source* source) { delete source; });
  if (!strcmp(name, "PCM_Source_BuildPeaks")) return reinterpret_cast<void*>(+[](PCM_source*, int mode) {
    on_main(); if (mode == 0) { ++builds; steps = 0; return 1; }
    if (mode == 2) { ++finishes; return 0; } return ++steps < 3 ? 50 : 0;
  });
  if (!strcmp(name, "PCM_Source_GetPeaks")) return reinterpret_cast<void*>(+[](PCM_source*, double rate, double start, int channels, int points, int extra, double* buffer) {
    on_main(); CHECK(channels == 2 && points == 4 && rate == 4 && start == 0.5 && extra == 0);
    for (int i = 0; i < points * channels; ++i) { buffer[i] = i / 10.0; buffer[points * channels + i] = -i / 10.0; }
    return peak_mode ? 0 : points;
  });
  if (!strcmp(name, "GetThemeColor")) return reinterpret_cast<void*>(+[](const char*, int flags) { on_main(); CHECK(flags == 0); return 0x123456; });
  if (!strcmp(name, "ColorFromNative")) return reinterpret_cast<void*>(+[](int, int* r, int* g, int* b) { on_main(); *r = 0x12; *g = 0x34; *b = 0x56; });
  return nullptr;
}
template<class F> void rejects(const char* code, F fn) { try { fn(); } catch (const Error& error) { CHECK(error.code == code); return; } throw std::runtime_error("Expected error"); }
}
int main() {
  const auto root = fs::current_path() / ("audio-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    fs::create_directories(root); std::ofstream(root / "audio.wav") << "source fixture";
    Host host; host.native_function = resolve;
    host.current_project = []() -> void* { return &project; };
    host.track_count = [] { return 2; };
    host.track_identity = [](int i) { return valid_track && i == track_index ? "{selected-guid}" : "{other-guid}"; };
    host.get_selected_track = [](void*, int) -> void* { return selected ? &track : nullptr; };
    uint64_t epoch = 1;
    Bridge::Controls controls;
    controls.project_epoch = [&] { return epoch; };
    Bridge bridge(host, controls);
    auto call = [&](const char* method, Json args) { return bridge.dispatch(Json{{"id", 1}, {"document", "audio"}, {"method", method}, {"args", args}}.dump()); };
    auto ref = call("ReaWeb_GetSelectedTrackRef", Json::array()).at("result");
    auto meter = call("ReaWeb_GetTrackMeter", Json::array({ref}))["result"];
    CHECK(meter["peak"][0] == 0.5 && meter["peakDb"][1].is_null());
    CHECK(std::abs(meter["peakDb"][0].get<double>() + 6.020599913) < 1e-8);
    CHECK(call("ReaWeb_GetTrackMeter", {nullptr})["error"]["code"] == "INVALID_ARGUMENT");
    for (int mode : {0, 1, 3}) {
      auto response = call("ReaWeb_FileDialog", {mode, "Title", u8"选择", "Audio|*.wav"});
      CHECK(response.contains("result") && response["result"] == (mode ? Json(u8"/选择/音频") : Json(nullptr)));
    }
    CHECK(cleared == 3 && call("ReaWeb_FileDialog", {2, "", "", ""})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(ref.at("projectEpoch") == epoch && ref.at("index") == 0 && ref.at("guid") == "{selected-guid}");
    selected = false;
    CHECK(call("ReaWeb_GetSelectedTrackRef", Json::array()).at("result").is_null());
    track_index = 1;
    CHECK(call("ReaWeb_GetTrackMeter", Json::array({ref})).at("result") == meter);
    CHECK(call("ReaWeb_SetTrackValue", {ref, "D_VOL", .5}).at("result") == true);
    CHECK(call("ReaWeb_SetTrackValue", {ref, "I_CUSTOMCOLOR", 1})["error"]["code"] == "INVALID_ARGUMENT");
    ++epoch;
    CHECK(call("ReaWeb_SetTrackValue", {ref, "D_VOL", .5})["error"]["code"] == "PROJECT_CHANGED");
    ref["projectEpoch"] = epoch;
    valid_track = false;
    CHECK(call("ReaWeb_SetTrackValue", {ref, "D_VOL", .5})["error"]["code"] == "STALE_HANDLE");
    CHECK(writes == 1);
    valid_track = true;
    int undo_steps = 0;
    host.begin_undo = [&](void* p) { CHECK(p == &project && undo_steps == 0); ++undo_steps; };
    host.end_undo = [&](void* p, const std::string& label) { CHECK(p == &project && label == "Gain" && undo_steps == 1); ++undo_steps; };
    const Json mix = {{"name", "Track"}, {"volume", .5}};
    auto read_mix = call("ReaWeb_GetTrackMix", Json::array({ref}));
    if (read_mix.contains("error")) throw std::runtime_error(read_mix.dump());
    CHECK(read_mix.at("result") == mix);
    auto apply_gain = call("ReaWeb_ApplyTrackGain", {ref, .5, "Gain"});
    if (apply_gain.contains("error")) throw std::runtime_error(apply_gain.dump());
    CHECK(apply_gain.at("result") == mix);
    CHECK(call("ReaWeb_ApplyTrackGain", {ref, -1, "Gain"})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(call("ReaWeb_TransportControl", {"Main_OnCommand"})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(undo_steps == 2 && writes == 2);
    valid_track = false;
    CHECK(call("ReaWeb_GetTrackMeter", Json::array({ref}))["error"]["code"] == "STALE_HANDLE");
    { AudioJob info(host, root, {"audio.wav"}, false); auto value = info.step(); CHECK(value && (*value)["sampleRate"] == 48000 && (*value)["bitDepth"] == 24); }
    CHECK(alive == 0);
    const Json options{{"points", 4}, {"start", 0.5}, {"duration", 1}};
    { AudioJob job(host, root, {"audio.wav", options}, true);
      CHECK(!job.step()); CHECK(!job.step()); CHECK(!job.step()); auto value = job.step(); CHECK(value);
      CHECK((*value)["points"] == 4 && (*value)["data"].size() == 2);
      CHECK((*value)["data"][1]["max"][3] == 0.7 && (*value)["data"][0]["min"][2] == -0.4);
    }
    CHECK(alive == 0 && builds == 1 && finishes == 1);
    { AudioJob cancelled(host, root, {"audio.wav", options}, true); CHECK(!cancelled.step()); }
    CHECK(alive == 0 && finishes == 2);
    peak_mode = 1;
    rejects("AUDIO_PEAKS_UNAVAILABLE", [&] { AudioJob job(host, root, {"audio.wav", options}, true); while (!job.step()) {} });
    CHECK(alive == 0);
    rejects("INVALID_ARGUMENT", [&] { AudioJob bad(host, root, {"audio.wav", {{"points", 0}}}, true); });
    rejects("INVALID_ARGUMENT", [&] { AudioJob bad(host, root, {"audio.wav", {{"duration", 3}}}, true); });
    rejects("FILE_NOT_FOUND", [&] { AudioJob bad(host, root, {"missing.wav"}, false); });
    CHECK(alive == 0);
    CHECK(theme_colors(host)["cssVariables"]["--reaper-background"] == "#123456");
    Host missing; CHECK(theme_colors(missing)["available"] == false);
    rejects("API_UNAVAILABLE", [&] { AudioJob bad(missing, root, {"audio.wav"}, false); });
    fs::remove_all(root);
    std::cout << "Audio: metadata, multichannel peaks, stepped generation, cancellation, errors and theme colors passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; fs::remove_all(root); return 1; }
}
