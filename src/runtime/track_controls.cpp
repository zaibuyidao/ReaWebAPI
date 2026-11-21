#include "runtime/track_controls.hpp"
#include "platform/shared/jsfx_editor.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>

namespace reaweb {
namespace {
double monotonic() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr const char* keys[] = {"volume", "pan", "mute", "solo"};
constexpr const char* fields[] = {"D_VOL", "D_PAN", "B_MUTE", "I_SOLO"};
std::string canonical(const Json& input) {
  if (!input.is_string()) throw Error("INVALID_ARGUMENT", "Expected a GUID");
  auto s = input.get<std::string>();
  bool valid = s.size() == 38 && s.front() == '{' && s.back() == '}';
  for (size_t i = 1; valid && i < 37; ++i) {
    auto& c = s[i];
    if (i == 9 || i == 14 || i == 19 || i == 24) valid = c == '-';
    else {
      if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
      valid = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
    }
  }
  if (!valid) throw Error("INVALID_ARGUMENT", "Expected a braced GUID");
  return s;
}
void append64(std::vector<unsigned char>& bytes, uint64_t bits) {
  for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<unsigned char>(bits >> (8 * i)));
}
void appendDouble(std::vector<unsigned char>& bytes, double v) {
  uint64_t bits; std::memcpy(&bits, &v, sizeof(bits)); append64(bytes, bits);
}
}
TrackControls::TrackControls(const Host& host, Source source) : host_(host), source_(source) {
  if (host.track_identity && host.native_function) {
    track_ = reinterpret_cast<decltype(track_)>(host.native_function("GetTrack"));
    if (source_ == Source::fxParameters) {
      fxCount_ = reinterpret_cast<decltype(fxCount_)>(host.native_function("TrackFX_GetCount"));
      recCount_ = reinterpret_cast<decltype(recCount_)>(host.native_function("TrackFX_GetRecCount"));
      takeCount_ = reinterpret_cast<decltype(takeCount_)>(host.native_function("TakeFX_GetCount"));
      containerApi_ = host.native_function("TrackFX_GetNamedConfigParm") && host.native_function("TakeFX_GetNamedConfigParm");
      trackParams_.count = reinterpret_cast<decltype(trackParams_.count)>(host.native_function("TrackFX_GetNumParams"));
      trackParams_.normalized = reinterpret_cast<decltype(trackParams_.normalized)>(host.native_function("TrackFX_GetParamNormalized"));
      takeParams_.count = reinterpret_cast<decltype(takeParams_.count)>(host.native_function("TakeFX_GetNumParams"));
      takeParams_.normalized = reinterpret_cast<decltype(takeParams_.normalized)>(host.native_function("TakeFX_GetParamNormalized"));
    } else value_ = reinterpret_cast<decltype(value_)>(host.native_function("GetMediaTrackInfo_Value"));
  }
}
bool TrackControls::available() const {
  const bool getters = source_ == Source::controls ? value_ != nullptr : (host_.fx_identity && fxCount_ && trackParams_.count && trackParams_.normalized);
  return track_ && getters && host_.current_project && host_.track_count && host_.track_identity;
}
void TrackControls::begin() {
  now_ = monotonic();
  auto* project = static_cast<ReaProject*>(host_.current_project());
  const auto gen = host_.project_generation ? host_.project_generation() : 0;
  if (!epoch_ || project != project_ || gen != generation_) ++epoch_;
  project_ = project; generation_ = gen;
  const auto list = host_.event_revision ? host_.event_revision("track-list") : 0;
  listChanged_ = list != listRevision_; listRevision_ = list;
  revision_ = host_.event_revision ? host_.event_revision(source_ == Source::controls ? "track-controls" : "fx-parameters") : 0;
  count_ = project ? host_.track_count() : 0;
  readings_.clear(); index_.clear(); searched_ = 0; queried_ = false;
}
TrackControls::Reading& TrackControls::resolve(const std::string& guid, int hint) {
  auto entry = readings_.try_emplace(guid);
  auto& r = entry.first->second;
  if (!entry.second) return r;
  if (source_ == Source::fxParameters && hint < 0 && host_.master_by_guid) {
    r.track = static_cast<MediaTrack*>(host_.master_by_guid(project_, guid));
    if (r.track) { r.master = true; return r; }
  }
  if (hint >= 0 && hint < count_ && host_.track_identity(hint) == guid) r.index = hint;
  else {
    while (!index_.count(guid) && searched_ < count_) {
      index_.emplace(host_.track_identity(searched_), searched_);
      ++searched_;
    }
    auto i = index_.find(guid);
    if (i != index_.end()) r.index = i->second;
  }
  if (r.index >= 0) r.track = track_(project_, r.index);
  return r;
}
TrackControls::FxReading& TrackControls::resolveFx(Reading& r, Watch& w) {
  const bool take = w.chain == "take";
  const bool input = w.chain == "input" || w.chain == "monitoring";
  auto [chainIt, newChain] = r.chains.try_emplace({w.chain, w.takeGuid});
  auto& chain = chainIt->second;
  if (newChain && r.track && (!input || r.master == (w.chain == "monitoring"))) {
    chain.owner = take ? host_.take_by_guid(project_, r.track, w.takeGuid) : r.track;
  }
  auto [it, added] = chain.effects.try_emplace(w.fxGuid);
  auto& fx = it->second;
  if (added && chain.owner) {
    fx.owner = chain.owner;
    const int count = take ? takeCount_(fx.owner) : input ? recCount_(r.track) : fxCount_(r.track);
    const auto matches = [&](int i) { return host_.fx_identity(fx.owner, i, take) == w.fxGuid; };
    const int flag = input ? 0x1000000 : 0;
    const int hint = w.fxIndex;
    if (hint >= 0 && (hint & 0x1000000) == flag &&
        ((hint & 0x2000000) || (hint & ~flag) < count) && matches(hint)) fx.index = hint;
    else {
      if (!chain.indices) {
        if (containerApi_) {
          JsfxTarget target{r.track, take ? fx.owner : nullptr, input};
          chain.indices = target.indices(host_);
        } else {
          chain.indices.emplace();
          for (int i = 0; i < count; ++i) chain.indices->push_back(i | flag);
        }
      }
      for (const int i : *chain.indices) if (matches(i)) { fx.index = i; break; }
    }
    if (fx.index >= 0) fx.parameterCount = (take ? takeParams_ : trackParams_).count(fx.owner, fx.index);
  }
  w.fxIndex = fx.index;
  return fx;
}
TrackControls::Watch TrackControls::watchFx(const Json& options) {
  Watch w;
  w.guids.push_back(canonical(options.value("track", Json())));
  w.fxGuid = canonical(options.value("fx", Json()));
  if (options.contains("chain")) {
    if (!options["chain"].is_string()) throw Error("INVALID_ARGUMENT", "Expected an FX chain name");
    w.chain = options["chain"].get<std::string>();
  }
  if (w.chain != "main" && w.chain != "input" && w.chain != "monitoring" && w.chain != "take")
    throw Error("INVALID_ARGUMENT", "Unknown FX chain");
  if (w.chain == "take") w.takeGuid = canonical(options.value("take", Json()));
  else if (options.contains("take")) throw Error("INVALID_ARGUMENT", "Take GUID requires the take chain");
  if ((w.chain == "take" && !takeAvailable()) || (w.chain == "input" && !inputAvailable()) ||
      (w.chain == "monitoring" && !monitoringAvailable()))
    throw Error("REALTIME_UNAVAILABLE", "Requested FX chain unavailable on this host");
  const auto params = options.value("parameters", Json());
  if (!params.is_array() || params.empty() || params.size() > 32)
    throw Error("INVALID_ARGUMENT", "Expected 1..32 FX parameter indices");
  for (const auto& p : params) {
    if (!p.is_number_integer() || p.get<double>() < 0 || p.get<double>() > std::numeric_limits<int>::max())
      throw Error("INVALID_ARGUMENT", "Invalid FX parameter index");
    const int index = p.get<int>();
    if (std::find(w.parameters.begin(), w.parameters.end(), index) != w.parameters.end())
      throw Error("INVALID_ARGUMENT", "Duplicate FX parameter index");
    w.parameters.push_back(index);
  }
  begin();
  w.epoch = epoch_;
  auto& track = resolve(w.guids.front(), -1);
  w.indices.push_back(track.index);
  const auto fx = resolveFx(track, w);
  end();
  if (fx.index < 0) throw Error("STALE_HANDLE", "FX is absent from the requested chain");
  if (*std::max_element(w.parameters.begin(), w.parameters.end()) >= fx.parameterCount)
    throw Error("INVALID_ARGUMENT", "Parameter is absent from this FX");
  return w;
}
TrackControls::Watch TrackControls::watch(const Json& options) {
  Watch w;
  if (!options.contains("tracks") || !options["tracks"].is_array() ||
      options["tracks"].empty() || options["tracks"].size() > 16)
    throw Error("INVALID_ARGUMENT", "Expected 1..16 track GUIDs");
  for (const auto& input : options["tracks"]) {
    auto guid = canonical(input);
    if (std::find(w.guids.begin(), w.guids.end(), guid) != w.guids.end())
      throw Error("INVALID_ARGUMENT", "Duplicate track GUID");
    w.guids.push_back(std::move(guid));
  }
  if (options.contains("properties")) {
    const auto& props = options["properties"];
    if (!props.is_array() || props.empty() || props.size() > 4)
      throw Error("INVALID_ARGUMENT", "Expected 1..4 track properties");
    for (const auto& prop : props) {
      unsigned i = 0;
      while (i < 4 && prop != keys[i]) ++i;
      if (i == 4 || (w.mask & (1u << i))) throw Error("INVALID_ARGUMENT", "Unknown or duplicate track property");
      w.mask |= 1u << i;
    }
  } else w.mask = 15;
  begin(); w.epoch = epoch_;
  for (const auto& guid : w.guids) {
    auto& r = resolve(guid, -1);
    if (!r.track) { end(); throw Error("STALE_HANDLE", "Track is absent from the active project"); }
    w.indices.push_back(r.index);
  }
  end();
  return w;
}
int TrackControls::sample(Watch& w, double rate, StreamHub& streams, uint64_t handle,
                          uint64_t& sequence, uint64_t& published, uint64_t& dropped) {
  if (w.epoch != epoch_) return stream_project_changed;
  const bool callback = w.samples && revision_ != w.revision;
  const double elapsed = now_ - w.lastSample;
  const bool due = !w.samples || (elapsed >= 1 / rate && (callback || elapsed >= 1.0 / 30));
  if (!due && !listChanged_) return 0;
  std::vector<double> values;
  for (size_t i = 0; i < w.guids.size(); ++i) {
    auto& r = resolve(w.guids[i], w.indices[i]);
    if (!r.track) { ++errors_; return stream_track_gone; }
    w.indices[i] = r.index;
    if (!w.fxGuid.empty()) {
      auto& fx = resolveFx(r, w);
      if (fx.index < 0 || *std::max_element(w.parameters.begin(), w.parameters.end()) >= fx.parameterCount) {
        ++errors_; return stream_track_gone;
      }
      if (due) for (const int index : w.parameters) {
        auto [it, added] = fx.values.try_emplace(index);
        if (added) it->second = (w.chain == "take" ? takeParams_ : trackParams_).normalized(fx.owner, fx.index, index);
        const double v = it->second;
        if (!std::isfinite(v) || v < 0 || v > 1) { ++errors_; return REAWEB_SERVICE_ERROR; }
        values.push_back(v);
      }
      continue;
    }
    if (!due) continue;
    for (unsigned k = 0; k < 4; ++k) {
      const auto bit = 1u << k;
      if (!(w.mask & bit)) continue;
      if (!(r.mask & bit)) { r.values[k] = value_(r.track, fields[k]); r.mask |= bit; }
      if (!std::isfinite(r.values[k])) { ++errors_; return REAWEB_SERVICE_ERROR; }
      values.push_back(r.values[k]);
    }
  }
  if (!due) return 0;
  if (!queried_) {
    if (lastSample_) maxInterval_ = std::max(maxInterval_, now_ - lastSample_);
    lastSample_ = now_; ++sourceSequence_; queried_ = true;
    if (callback) ++callbacks_; else ++snapshots_;
  }
  if (w.samples) w.missed += static_cast<uint64_t>(std::max(0.0, std::floor(elapsed * rate) - 1));
  else w.firstSample = now_;
  w.trigger = !w.samples ? "initial" : callback ? "control-surface" : "periodic";
  ++w.samples; w.lastSample = now_; w.revision = revision_;
  w.durationMs = (monotonic() - now_) * 1000;
  if (w.previous == values) { ++w.unchanged; return 0; }
  std::vector<unsigned char> bytes{'T','C','0','1', static_cast<unsigned char>(w.mask),
                                  static_cast<unsigned char>(w.guids.size()), 0, 0};
  if (!w.fxGuid.empty()) bytes = {'F','P','0','1', static_cast<unsigned char>(w.parameters.size()),0,0,0};
  appendDouble(bytes, now_); appendDouble(bytes, w.durationMs);
  append64(bytes, w.epoch); append64(bytes, sourceSequence_);
  for (double v : values) appendDouble(bytes, v);
  if (streams.publish(REAWEB_BINARY, handle, bytes.data(), static_cast<uint32_t>(bytes.size()), ++sequence, now_) == REAWEB_OK) {
    w.previous = std::move(values); ++published; w.lastPublish = monotonic();
    w.sampleToPublishMs = (w.lastPublish - now_) * 1000;
  } else ++dropped;
  return 0;
}
Json TrackControls::diagnostics() const {
  return {{"sampled",sourceSequence_},{"errors",errors_},{"lastSampleAt",lastSample_},
    {"maxSampleIntervalMs",maxInterval_*1000},{"callbackTriggered",callbacks_},{"periodicSnapshots",snapshots_}};
}
}
