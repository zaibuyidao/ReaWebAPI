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
  if (!strcmp(name, "GetTrack")) return reinterpret_cast<void*>(+[](void*, int) -> void* { return &track; });
  if (!strcmp(name, "GetTrackGUID")) return reinterpret_cast<void*>(+[](void*) -> GUID* { return &track_guid; });
  if (!strcmp(name, "ValidatePtr2")) return reinterpret_cast<void*>(+[](void*, void* value, const char*) { return value == &project || (valid_track && value == &track); });
  if (!strcmp(name, "GetMediaTrackInfo_Value")) return reinterpret_cast<void*>(+[](void* value, const char* key) { on_main(); CHECK(value == &track && !strcmp(key, "I_NCHAN")); return 2.0; });
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
    Bridge bridge(host, {}, "audio-tests");
    auto call = [&](const char* method, Json args) { return bridge.dispatch(Json{{"id", 1}, {"document", "audio"}, {"method", method}, {"args", args}}.dump()); };
    auto handle = call("GetTrack", {0, 0})["result"];
    auto meter = call("ReaWeb_GetTrackMeter", Json::array({handle}))["result"];
    CHECK(meter["peak"][0] == 0.5 && meter["peakDb"][1].is_null());
    CHECK(std::abs(meter["peakDb"][0].get<double>() + 6.020599913) < 1e-8);
    CHECK(call("ReaWeb_GetTrackMeter", {nullptr})["error"]["code"] == "INVALID_HANDLE");
    valid_track = false;
    CHECK(call("ReaWeb_GetTrackMeter", Json::array({handle}))["error"]["code"] == "STALE_HANDLE");
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
