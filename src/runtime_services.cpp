#include "runtime_services.hpp"
#include <reaper_plugin.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <regex>

namespace reaweb {
std::string runtime_platform() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}
std::string runtime_architecture() {
#if defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
  return "x64";
#elif defined(_M_IX86) || defined(__i386__)
  return "x86";
#elif defined(__arm__)
  return "arm";
#else
  return "unknown";
#endif
}
fs::path existing_local_path(const fs::path& base, const Json& input) {
  if (!input.is_string()) throw Error("INVALID_PATH", "Expected a local path");
  const auto value = input.get<std::string>();
  if (value.empty() || value.size() > 32768 || value.find('\0') != std::string::npos || value.find("://") != std::string::npos)
    throw Error("INVALID_PATH", "Expected a local path without a URL scheme or NUL");
  auto path = fs::u8path(value); std::error_code error;
  path = fs::canonical(path.is_absolute() ? path : base / path, error);
  if (error) throw Error("FILE_NOT_FOUND", "Local path is unavailable: " + value);
  return path;
}
Json drag_payload(const fs::path& base, const std::string& method, const Json& args) {
  if (args.size() != 1) throw Error("INVALID_ARGUMENT", "Expected one drag argument");
  if (method == "ReaWeb_DragText") {
    if (!args[0].is_string()) throw Error("INVALID_ARGUMENT", "Expected drag text");
    const auto text = args[0].get<std::string>();
    if (text.empty() || text.size() > value_limit || text.find('\0') != std::string::npos)
      throw Error("INVALID_ARGUMENT", "Drag text must be nonempty UTF-8 without NUL, at most 16 MiB");
    return {{"files", Json::array()}, {"text", text}};
  }
  if (!args[0].is_array() || args[0].empty() || args[0].size() > 256)
    throw Error("INVALID_ARGUMENT", "Expected 1..256 existing file paths");
  Json files = Json::array();
  for (const auto& value : args[0]) {
    auto path = existing_local_path(base, value);
    if (!fs::is_regular_file(path)) throw Error("INVALID_PATH", "Drag sources must be regular files");
    const auto text = path.u8string();
    if (std::find(files.begin(), files.end(), text) == files.end()) files.push_back(text);
  }
  return {{"files", files}, {"text", ""}};
}
Json app_info(const fs::path& root, const fs::path& data, const std::string& id) {
  Json result = {{"id", id}, {"name", root.filename().u8string()}, {"version", nullptr},
    {"rootPath", fs::canonical(root).u8string()}, {"dataPath", fs::absolute(data).lexically_normal().u8string()}};
  const auto path = root / "app.json";
  if (fs::exists(path)) {
    if (!fs::is_regular_file(path) || fs::file_size(path) > 65536) throw Error("APP_MANIFEST_INVALID", "app.json exceeds 64 KiB or is not a file");
    std::ifstream stream(path, std::ios::binary);
    auto manifest = Json::parse(stream, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()) throw Error("APP_MANIFEST_INVALID", "app.json must contain a JSON object");
    for (const auto* field : {"name", "version"}) if (manifest.contains(field)) {
      if (!manifest[field].is_string()) throw Error("APP_MANIFEST_INVALID", std::string("Invalid App ") + field);
      const auto value = manifest[field].get<std::string>();
      if (value.empty() || value.find('\0') != std::string::npos ||
          (std::string(field) == "name" && (std::count_if(value.begin(), value.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; }) > 256 || value.find_first_not_of(" \t\r\n") == std::string::npos)) ||
          (std::string(field) == "version" && !std::regex_match(value, std::regex("[0-9]+\\.[0-9]+\\.[0-9]+(?:-[A-Za-z0-9.-]+)?"))))
        throw Error("APP_MANIFEST_INVALID", std::string("Invalid App ") + field);
      result[field] = value;
    }
  }
  std::error_code error; fs::create_directories(data, error);
  if (error) throw Error("APP_DATA_UNAVAILABLE", "Cannot create the App data directory: " + error.message());
  result["dataPath"] = fs::canonical(data).u8string();
  // Verify write access even when a previous installation created this directory.
  fs::path probe;
  for (int attempt = 0; attempt < 4; ++attempt) {
    auto candidate = data / (".write-check-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(attempt));
    if (fs::create_directory(candidate, error)) { probe = candidate; break; }
    if (error) break;
  }
  if (probe.empty()) throw Error("APP_DATA_UNAVAILABLE", "App data directory is not writable");
  struct RemoveProbe { fs::path path; ~RemoveProbe() { std::error_code ignored; fs::remove(path / "check", ignored); fs::remove(path, ignored); } } cleanup{probe};
  std::ofstream check(probe / "check", std::ios::binary); check << '1'; check.close();
  if (!check) throw Error("APP_DATA_UNAVAILABLE", "Cannot write to the App data directory");
  return result;
}
namespace {
template<class T> T require(const Host& host, const char* name) {
  auto fn = host.native_function ? host.native_function(name) : nullptr;
  if (!fn) throw Error("API_UNAVAILABLE", std::string("REAPER API unavailable: ") + name);
  return reinterpret_cast<T>(fn);
}
}
const std::vector<std::string>& runtime_events() {
  static const std::vector<std::string> names = {"projectchange", "selectionchange", "itemselectionchange",
    "takeselectionchange", "transportchange", "fxchange", "windowstatechange", "track-added", "track-deleted",
    "track-selected", "item-changed", "take-changed", "playback-state-changed", "tempo-changed",
    "marker-changed", "fx-changed", "project-loaded", "project-saved", "theme-changed", "native-drop"};
  return names;
}
Json theme_colors(const Host& host) {
  auto get = host.native_function ? reinterpret_cast<int (*)(const char*, int)>(host.native_function("GetThemeColor")) : nullptr;
  auto convert = host.native_function ? reinterpret_cast<void (*)(int, int*, int*, int*)>(host.native_function("ColorFromNative")) : nullptr;
  Json colors, variables;
  const char* names[] = {"background", "text", "highlight", "panel", "border"};
  const char* keys[] = {"col_main_bg2", "col_main_text2", "col_seltrack2", "col_main_bg", "col_main_3dsh"};
  const char* fallback[] = {"#303030", "#eeeeee", "#487ca5", "#383838", "#202020"};
  bool available = get && convert;
  for (int i = 0; i < 5; ++i) {
    std::string css = fallback[i];
    const auto value = get && convert ? get(keys[i], 0) : -1;
    if (value != -1) {
      int r = 0, g = 0, b = 0; convert(value, &r, &g, &b);
      char text[8]; std::snprintf(text, sizeof(text), "#%02x%02x%02x", r & 255, g & 255, b & 255); css = text;
    }
    colors[names[i]] = css; variables[std::string("--reaper-") + names[i]] = css;
  }
  return {{"available", available}, {"colors", colors}, {"cssVariables", variables}};
}

struct AudioJob::Impl {
  PCM_source* source = nullptr;
  void (*destroy)(PCM_source*) = nullptr;
  int (*build)(PCM_source*, int) = nullptr;
  int (*peaks)(PCM_source*, double, double, int, int, int, double*) = nullptr;
  bool waveform = false, building = false, begun = false;
  Json info;
  int points = 1024, channels = 0;
  double start = 0, length = 0;
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  ~Impl() {
    if (source) { if (building) build(source, 2); destroy(source); }
  }
};
AudioJob::AudioJob(const Host& host, const fs::path& base, const Json& args, bool waveform) : impl_(std::make_unique<Impl>()) {
  auto& job = *impl_; job.waveform = waveform;
  if (!args.is_array() || args.empty() || args.size() > (waveform ? 2u : 1u) || !args[0].is_string())
    throw Error("INVALID_ARGUMENT", "Expected an audio path and optional waveform options");
  const auto name = args[0].get<std::string>();
  if (name.empty() || name.size() > 32768 || name.find('\0') != std::string::npos)
    throw Error("INVALID_PATH", "Invalid audio file path");
  const auto path = fs::absolute(base / fs::u8path(name)).lexically_normal();
  if (!fs::is_regular_file(path)) throw Error("FILE_NOT_FOUND", "Audio file does not exist");
  const Json options = args.size() == 2 ? args[1] : Json::object();
  if (!options.is_object()) throw Error("INVALID_ARGUMENT", "Expected waveform options");
  for (const auto& field : options.items()) if (field.key() != "points" && field.key() != "start" && field.key() != "duration")
    throw Error("INVALID_ARGUMENT", "Unknown waveform option: " + field.key());
  if (options.contains("points")) {
    if (!options["points"].is_number_integer() || options["points"].get<double>() < 1 || options["points"].get<double>() > 8192)
      throw Error("INVALID_ARGUMENT", "Waveform points must be an integer from 1 to 8192");
    job.points = options["points"].get<int>();
  }
  for (const char* key : {"start", "duration"}) if (options.contains(key) &&
      (!options[key].is_number() || !std::isfinite(options[key].get<double>()) || options[key].get<double>() < 0))
    throw Error("INVALID_ARGUMENT", "Waveform time must be finite and non-negative");
  auto create = require<PCM_source* (*)(const char*, bool)>(host, "PCM_Source_CreateFromFileEx");
  job.destroy = require<void (*)(PCM_source*)>(host, "PCM_Source_Destroy");
  if (waveform) {
    job.build = require<int (*)(PCM_source*, int)>(host, "PCM_Source_BuildPeaks");
    job.peaks = require<int (*)(PCM_source*, double, double, int, int, int, double*)>(host, "PCM_Source_GetPeaks");
  }
  job.source = create(path.u8string().c_str(), true);
  if (!job.source || !job.source->IsAvailable()) throw Error("AUDIO_UNSUPPORTED", "REAPER cannot open this audio file");
  const auto rate = job.source->GetSampleRate(), length = job.source->GetLength();
  job.channels = job.source->GetNumChannels();
  if (!std::isfinite(rate) || rate <= 0 || !std::isfinite(length) || length < 0 || job.channels < 1 || job.channels > 32)
    throw Error("AUDIO_UNSUPPORTED", "Expected an audio source with 1 to 32 channels");
  const int bits = job.source->GetBitsPerSample();
  job.info = {{"path", path.u8string()}, {"sampleRate", rate}, {"channels", job.channels},
    {"bitDepth", bits > 0 ? Json(bits) : Json()}, {"duration", length},
    {"format", job.source->GetType() ? job.source->GetType() : ""}};
  job.start = options.value("start", 0.0);
  job.length = options.value("duration", std::max(0.0, length - job.start));
  if (job.start > length || job.length <= 0 || job.length > length - job.start + 1e-9) {
    if (waveform) throw Error("INVALID_ARGUMENT", "Waveform range must be within a non-empty audio file");
  }
}
AudioJob::~AudioJob() = default;
std::optional<Json> AudioJob::step() {
  auto& job = *impl_;
  if (!job.waveform) return job.info;
  if (std::chrono::steady_clock::now() >= job.deadline) throw Error("AUDIO_TIMEOUT", "Waveform generation exceeded 120 seconds");
  if (!job.begun) { job.begun = true; job.building = job.build(job.source, 0) != 0; if (job.building) return {}; }
  if (job.building) {
    if (job.build(job.source, 1)) return {};
    job.build(job.source, 2); job.building = false;
  }
  const double peak_rate = job.points / job.length;
  if (!std::isfinite(peak_rate)) throw Error("INVALID_ARGUMENT", "Waveform range is too small");
  const size_t block = static_cast<size_t>(job.points) * job.channels;
  std::vector<double> buffer(block * 2);
  const auto result = job.peaks(job.source, peak_rate, job.start, job.channels, job.points, 0, buffer.data());
  const int count = result & 0xfffff;
  if (count < 1 || count > job.points || (result & 0xf00000)) throw Error("AUDIO_PEAKS_UNAVAILABLE", "REAPER did not return standard waveform peaks");
  Json data = Json::array();
  for (int channel = 0; channel < job.channels; ++channel) {
    Json low = Json::array(), high = Json::array();
    for (int i = 0; i < count; ++i) {
      const auto at = static_cast<size_t>(i) * job.channels + channel;
      const auto minimum = buffer[block + at], maximum = buffer[at];
      if (!std::isfinite(minimum) || !std::isfinite(maximum)) throw Error("AUDIO_INVALID_DATA", "Non-finite peak value");
      low.push_back(minimum); high.push_back(maximum);
    }
    data.push_back({{"min", low}, {"max", high}});
  }
  auto output = job.info;
  output.update({{"start", job.start}, {"rangeDuration", job.length}, {"points", count},
    {"requestedPoints", job.points}, {"peaksPerSecond", peak_rate}, {"data", data}});
  return output;
}
}
