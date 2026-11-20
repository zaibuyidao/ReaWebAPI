#include "core/core.hpp"
#include <reaper_plugin.h>
#include <cmath>
#include <cstring>

namespace reaweb {
namespace {
template<class Fn> Fn required(const Host& host, const char* name) {
  auto address = host.native_function ? host.native_function(name) : nullptr;
  if (!address) throw Error("API_UNAVAILABLE", std::string(name) + " is unavailable");
  return reinterpret_cast<Fn>(address);
}

Json file_dialog(const Host& host, const Json& args) {
  if (!args[0].is_number_integer() || (args[0] != 0 && args[0] != 1 && args[0] != 3))
    throw Error("INVALID_ARGUMENT", "Expected save, open or folder mode");
  for (size_t i = 1; i < args.size(); ++i) {
    if (!args[i].is_string()) throw Error("INVALID_ARGUMENT", "Expected a dialog string");
    const auto& s = args[i].get_ref<const std::string&>();
    if (s.size() > value_limit) throw Error("BUFFER_LIMIT", "String exceeds 16 MiB");
    if (s.find('\0') != std::string::npos) throw Error("INVALID_ARGUMENT", "Embedded NUL is not allowed");
  }
  auto dialog = required<bool (*)(int, const char*, const char*, const char*, char*, int)>(host, "GetUserFileName");
  auto reg = required<int (*)(char**, int*)>(host, "realloc_cmd_register_buf");
  auto clear = required<void (*)(int)>(host, "realloc_cmd_clear");
  std::vector<char> initial(65536);
  char* path = initial.data();
  int capacity = static_cast<int>(initial.size());
  struct Registration {
    void (*clear)(int);
    int token;
    ~Registration() { clear(token); }
  } registration{clear, reg(&path, &capacity)};
  if (!dialog(args[0].get<int>(), args[1].get_ref<const std::string&>().c_str(),
              args[2].get_ref<const std::string&>().c_str(), args[3].get_ref<const std::string&>().c_str(),
              path, capacity)) return nullptr;
  if (!path || capacity <= 0 || size_t(capacity) > value_limit ||
      (path == initial.data() && size_t(capacity) > initial.size()))
    throw Error("BUFFER_LIMIT", "Invalid dialog result size");
  auto end = static_cast<const char*>(std::memchr(path, 0, capacity));
  if (!end || (path == initial.data() && end == path + initial.size() - 1))
    throw Error("BUFFER_LIMIT", "Dialog result filled its buffer");
  return std::string(path, static_cast<size_t>(end - path));
}
Json track_mix(const Host& host, MediaTrack* track) {
  auto name = required<bool (*)(MediaTrack*, char*, int)>(host, "GetTrackName");
  auto info = required<double (*)(MediaTrack*, const char*)>(host, "GetMediaTrackInfo_Value");
  char title[65536]{};
  if (!name(track, title, sizeof(title))) throw Error("STALE_HANDLE", "Track name is unavailable");
  return {{"name", title}, {"volume", info(track, "D_VOL")}};
}
}

std::string host_version(const Host& host) {
  auto version = required<const char* (*)()>(host, "GetAppVersion")();
  if (!version) throw Error("API_UNAVAILABLE", "Host version is unavailable");
  return version;
}

Json project_summary(const Host& host) {
  auto project = static_cast<ReaProject*>(host.current_project());
  auto name = required<void (*)(ReaProject*, char*, int)>(host, "GetProjectName");
  auto count = required<int (*)(ReaProject*)>(host, "CountTracks");
  auto state = required<int (*)()>(host, "GetPlayState");
  char title[65536]{};
  name(project, title, sizeof(title));
  return {{"name", title}, {"trackCount", count(project)}, {"playState", state()}};
}

void* Bridge::audio_track(const Json& ref) {
  if (!ref.is_object() || !ref.contains("projectEpoch") || !ref["projectEpoch"].is_number_integer() ||
      !ref.contains("guid") || !ref["guid"].is_string())
    throw Error("INVALID_ARGUMENT", "Expected a project epoch and track GUID");
  const auto& guid = ref["guid"].get_ref<const std::string&>();
  if (guid.empty() || guid.size() > 64) throw Error("INVALID_ARGUMENT", "Invalid track GUID");
  if (!controls_.project_epoch || !host_.track_count || !host_.track_identity)
    throw Error("API_UNAVAILABLE", "Project track references are unavailable");
  if (ref["projectEpoch"] != controls_.project_epoch())
    throw Error("PROJECT_CHANGED", "The track belongs to an earlier project epoch");
  const int count = host_.track_count();
  auto get = required<MediaTrack* (*)(ReaProject*, int)>(host_, "GetTrack");
  auto project = static_cast<ReaProject*>(project_);
  // The index is only a hint. Resolve the same GUID again if the user reordered tracks.
  if (ref.contains("index") && ref["index"].is_number_integer() && ref["index"] >= 0 && ref["index"] < count) {
    int i = ref["index"].get<int>();
    if (host_.track_identity(i) == guid)
      if (auto track = get(project, i)) return track;
  }
  for (int i = 0; i < count; ++i)
    if (host_.track_identity(i) == guid)
      if (auto track = get(project, i)) return track;
  throw Error("STALE_HANDLE", "The track was deleted or replaced");
}

void Bridge::validate_managed_call(const std::string& method) {
  // Runtime already checked the gesture owner and queued project epoch.
  // GUID references are resolved against the current project at execution.
  if (method == "ReaWeb_GetSelectedTrackRef" || method == "ReaWeb_SetTrackValue" ||
      method == "ReaWeb_GetTrackMeter" || method == "ReaWeb_GetTrackMix") return;
  throw Error("UNDO_BUSY", "This operation is unavailable inside a managed Undo gesture");
}

void Bridge::add_runtime_calls() {
  add("ReaWeb_TransportControl", 1, 1, [this](const Json& a) -> Json {
    const char* command;
    if (a[0] == "play") command = "OnPlayButton";
    else if (a[0] == "pause") command = "OnPauseButton";
    else if (a[0] == "stop") command = "OnStopButton";
    else throw Error("INVALID_ARGUMENT", "Expected play, pause or stop");
    required<void (*)()>(host_, command)();
    return nullptr;
  });
  add("ReaWeb_GetTrackMix", 1, 1, [this](const Json& a) -> Json {
    return track_mix(host_, static_cast<MediaTrack*>(audio_track(a[0])));
  });
  add("ReaWeb_ApplyTrackGain", 3, 3, [this](const Json& a) -> Json {
    if (!a[0].contains("guid") || !a[1].is_number() || !std::isfinite(a[1].get<double>()) || a[1] < 0 || !a[2].is_string())
      throw Error("INVALID_ARGUMENT", "Expected a project track reference, linear gain and Undo label");
    const auto& label = a[2].get_ref<const std::string&>();
    if (label.empty() || label.size() > 256 || label.find('\0') != std::string::npos)
      throw Error("INVALID_ARGUMENT", "Invalid Undo label");
    if (!host_.begin_undo || !host_.end_undo) throw Error("UNDO_UNAVAILABLE", "Undo groups are unavailable");
    auto track = static_cast<MediaTrack*>(audio_track(a[0]));
    auto set = required<bool (*)(MediaTrack*, const char*, double)>(host_, "SetMediaTrackInfo_Value");
    host_.begin_undo(project_);
    bool applied = set(track, "D_VOL", a[1].get<double>());
    host_.end_undo(project_, label);
    if (!applied) throw Error("NATIVE_ERROR", "REAPER rejected the track gain");
    if (host_.update_arrange) host_.update_arrange();
    return track_mix(host_, track);
  });
  add("ReaWeb_FileDialog", 4, 4, [this](const Json& a) { return file_dialog(host_, a); });
  add("ReaWeb_GetSelectedTrackRef", 0, 0, [this](const Json&) -> Json {
    if (!controls_.project_epoch || !host_.get_selected_track || !host_.track_count || !host_.track_identity)
      throw Error("API_UNAVAILABLE", "Project track references are unavailable");
    auto track = host_.get_selected_track(project_, 0);
    if (!track) return nullptr;
    auto get = required<MediaTrack* (*)(ReaProject*, int)>(host_, "GetTrack");
    const int count = host_.track_count();
    for (int i = 0; i < count; ++i) {
      if (get(static_cast<ReaProject*>(project_), i) != track) continue;
      auto guid = host_.track_identity(i);
      if (!guid.empty()) return {{"projectEpoch", controls_.project_epoch()}, {"guid", std::move(guid)}, {"index", i}};
      break;
    }
    throw Error("STALE_HANDLE", "Selected track identity is unavailable");
  });
  add("ReaWeb_SetTrackValue", 3, 3, [this](const Json& a) {
    if ((a[1] != "D_VOL" && a[1] != "D_PAN" && a[1] != "B_MUTE" && a[1] != "I_SOLO") ||
        !a[2].is_number() || !std::isfinite(a[2].get<double>()))
      throw Error("INVALID_ARGUMENT", "Expected a continuous track key and finite value");
    auto track = static_cast<MediaTrack*>(audio_track(a[0]));
    auto set = required<bool (*)(MediaTrack*, const char*, double)>(host_, "SetMediaTrackInfo_Value");
    return set(track, a[1].get_ref<const std::string&>().c_str(), a[2].get<double>());
  });
  add("ReaWeb_GetTrackMeter", 1, 1, [this](const Json& a) {
    auto track = static_cast<MediaTrack*>(audio_track(a[0]));
    auto peak = required<double (*)(MediaTrack*, int)>(host_, "Track_GetPeakInfo");
    auto info = required<double (*)(MediaTrack*, const char*)>(host_, "GetMediaTrackInfo_Value");
    const double count = info(track, "I_NCHAN");
    if (!std::isfinite(count) || count < 1 || count > 128 || std::floor(count) != count)
      throw Error("AUDIO_UNSUPPORTED", "Invalid track channel count");
    Json values = Json::array(), db = Json::array();
    for (int channel = 0; channel < static_cast<int>(count); ++channel) {
      double value = peak(track, channel);
      if (!std::isfinite(value) || value < 0) throw Error("AUDIO_INVALID_DATA", "Invalid meter value");
      values.push_back(value); db.push_back(value > 0 ? Json(20 * std::log10(value)) : Json());
    }
    return Json{{"channels", static_cast<int>(count)}, {"peak", values}, {"peakDb", db}, {"unit", "linear-amplitude"}};
  });
}
}
