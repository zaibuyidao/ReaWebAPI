#include "runtime/services.hpp"
#include <reaper_plugin.h>
#include <algorithm>
#include <cmath>
#include <chrono>

namespace reaweb {
namespace {
template<class T> T require(const Host& host, const char* name) {
  auto fn = host.native_function ? host.native_function(name) : nullptr;
  if (!fn) throw Error("API_UNAVAILABLE", std::string("REAPER API unavailable: ") + name);
  return reinterpret_cast<T>(fn);
}
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
