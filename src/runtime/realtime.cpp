#include "runtime/realtime.hpp"
#include "runtime/track_controls.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reaweb {
namespace {
using Clock = std::chrono::steady_clock;
double seconds(Clock::time_point time) { return std::chrono::duration<double>(time.time_since_epoch()).count(); }
void put64(std::vector<unsigned char>& data, size_t at, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) data[at + i] = static_cast<unsigned char>(value >> (8 * i));
}
void put_double(std::vector<unsigned char>& data, size_t at, double value) {
  uint64_t bits; std::memcpy(&bits, &value, 8); put64(data, at, bits);
}
struct Sample { double timestamp; std::vector<unsigned char> bytes; };
class Source {
public:
  virtual ~Source() = default;
  virtual const char* name() const = 0;
  virtual const char* schema() const = 0;
  virtual bool available() const = 0;
  virtual void refresh_context() = 0; // Main thread only, serialized with read().
  virtual Sample read() = 0;
  virtual void reset() = 0;
};
class TransportSource final : public Source {
  const Host& host_;
  double (*position_)() = nullptr;
  int (*state_)() = nullptr;
  double (*rate_)(double, void*) = nullptr;
  void (*signature_)(void*, double, int*, int*, double*) = nullptr;
  double (*qn_)(void*, double) = nullptr;
  void* project_ = nullptr;
  uint64_t generation_ = 0, epoch_ = 0, published_epoch_ = 0, sequence_ = 0;
public:
  explicit TransportSource(const Host& host) : host_(host) {
    if (host.native_function) {
      position_ = reinterpret_cast<double (*)()>(host.native_function("GetPlayPosition"));
      state_ = reinterpret_cast<int (*)()>(host.native_function("GetPlayState"));
      rate_ = reinterpret_cast<double (*)(double, void*)>(host.native_function("Master_GetPlayRateAtTime"));
      signature_ = reinterpret_cast<void (*)(void*, double, int*, int*, double*)>(host.native_function("TimeMap_GetTimeSigAtTime"));
      qn_ = reinterpret_cast<double (*)(void*, double)>(host.native_function("TimeMap_timeToQN_abs"));
    }
  }
  const char* name() const override { return "transport"; }
  const char* schema() const override { return "transport.v2"; }
  bool available() const override { return position_ && state_ && rate_ && signature_ && qn_ && host_.current_project; }
  void reset() override { project_ = nullptr; }
  void refresh_context() override {
    auto* project = host_.current_project();
    const auto generation = host_.project_generation ? host_.project_generation() : 0;
    const bool changed = !epoch_ || project != project_ || generation != generation_;
    if (changed) { ++epoch_; project_ = project; generation_ = generation; }
  }
  Sample read() override {
    const auto begin = Clock::now();
    // Query every musical value at the same audible position.
    const auto before = state_();
    const auto position = position_();
    if (!project_ || !std::isfinite(position))
      throw Error("REALTIME_SAMPLE_UNAVAILABLE", "Transport position is unavailable");
    int numerator = 0, denominator = 0;
    double tempo = 0;
    const auto play_rate = rate_(position, nullptr);
    signature_(nullptr, position, &numerator, &denominator, &tempo);
    const auto beat_position = qn_(nullptr, position);
    const auto state = state_();
    if (!project_ || before != state || !std::isfinite(position) || state < 0 || state > 7 ||
        !std::isfinite(play_rate) || play_rate <= 0 || !std::isfinite(tempo) || tempo <= 0 ||
        !std::isfinite(beat_position) || numerator <= 0 || denominator <= 0)
      throw Error("REALTIME_SAMPLE_UNAVAILABLE", "Transport changed during sampling or is unavailable");
    const auto end = Clock::now();
    Sample sample{seconds(begin), std::vector<unsigned char>(96, 0)};
    auto& b = sample.bytes;
    b[0] = 'R'; b[1] = 'T'; b[2] = '0'; b[3] = '2';
    put_double(b, 8, sample.timestamp); put_double(b, 16, position);
    b[24] = static_cast<unsigned char>(state); b[28] = epoch_ != published_epoch_ ? 1 : 0;
    published_epoch_ = epoch_;
    put64(b, 32, epoch_); put_double(b, 40, seconds(end) - sample.timestamp);
    put64(b, 48, ++sequence_);
    put_double(b, 64, play_rate); put_double(b, 72, tempo); put_double(b, 80, beat_position);
    for (unsigned i = 0; i < 4; ++i) {
      b[88 + i] = static_cast<unsigned char>(static_cast<uint32_t>(numerator) >> (8 * i));
      b[92 + i] = static_cast<unsigned char>(static_cast<uint32_t>(denominator) >> (8 * i));
    }
    return sample;
  }
};
}
struct RealtimeManager::Impl {
  struct Subscription {
    uint64_t id, handle, sequence = 0, published = 0, dropped = 0;
    int window;
    double rate;
    std::string name, source;
    Clock::time_point created = Clock::now(), due{};
    bool attached = false;
    std::optional<TrackControls::Watch> tracks;
  };
  struct SourceState {
    std::unique_ptr<Source> source;
    uint64_t sampled = 0, errors = 0;
    double last_sample = 0, max_interval = 0;
  };
  TrackControls controls, fxParameters;
  TrackControls& mainSource(const std::string& name) {
    return name == "fx-parameters" ? fxParameters : controls;
  }
  StreamHub& streams;
  std::unique_ptr<RealtimeScheduler> scheduler;
  std::map<std::string, SourceState> sources;
  std::map<uint64_t, Subscription> subscriptions;
  uint64_t serial = 0;
  std::atomic<uint64_t> skipped{0};
  mutable std::mutex mutex;
  double rate = 0;
  bool stopped = false;
  std::thread::id main = std::this_thread::get_id();
  Impl(const Host& host, StreamHub& hub, std::unique_ptr<RealtimeScheduler> clock)
    : controls(host), fxParameters(host, TrackControls::Source::fxParameters), streams(hub), scheduler(clock ? std::move(clock) : make_realtime_scheduler()) {
    auto source = std::make_unique<TransportSource>(host);
    const std::string name = source->name(); sources.emplace(name, SourceState{std::move(source)});
  }
  void check() const { if (std::this_thread::get_id() != main) throw Error("WRONG_THREAD", "Realtime management requires the main thread"); }
  void reschedule() {
    double next = 0;
    for (const auto& item : subscriptions)
      if (!item.second.tracks) next = std::max(next, item.second.rate);
    if (next == rate) return;
    scheduler->stop(); rate = 0;
    if (next) { scheduler->start(next, [this] { sample(); }); rate = next; }
  }
  void sample() noexcept {
    // Never block behind management. stop() joins the worker while holding this lock.
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) { ++skipped; return; }
    if (stopped || !rate) return;
    const auto now = Clock::now();
    for (auto& pair : sources) {
      auto& source = pair.second;
      bool needed = false;
      // Assign each deadline to the nearest wake, tolerating phase jitter without catch-up bursts.
      const auto tolerance = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(0.5 / rate));
      for (const auto& item : subscriptions)
        if (item.second.source == pair.first && item.second.due <= now + tolerance) needed = true;
      if (!needed) continue;
      try {
        const auto value = source.source->read();
        if (source.last_sample) source.max_interval = std::max(source.max_interval, value.timestamp - source.last_sample);
        source.last_sample = value.timestamp; ++source.sampled;
        for (auto& item : subscriptions) {
          auto& sub = item.second;
          if (sub.source != pair.first || sub.due > now + tolerance) continue;
          const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1 / sub.rate));
          if (sub.due == Clock::time_point{}) sub.due = now;
          sub.due += period;
          if (sub.due <= now + tolerance) sub.due += period * ((now + tolerance - sub.due) / period + 1);
          const auto result = streams.publish(REAWEB_BINARY, sub.handle, value.bytes.data(), static_cast<uint32_t>(value.bytes.size()), ++sub.sequence, value.timestamp);
          if (result == REAWEB_OK) ++sub.published; else ++sub.dropped;
        }
      } catch (...) { ++source.errors; }
    }
  }
};
RealtimeManager::RealtimeManager(const Host& host, StreamHub& streams, std::unique_ptr<RealtimeScheduler> clock)
  : impl_(std::make_unique<Impl>(host, streams, std::move(clock))) {}
RealtimeManager::~RealtimeManager() { shutdown(); }
Json RealtimeManager::capabilities() const {
  auto& p = *impl_; p.check(); std::lock_guard<std::mutex> lock(p.mutex); Json sources = Json::object();
  for (const auto& item : p.sources) sources[item.first] = {{"available", p.scheduler->supported() && item.second.source->available()},
    {"schema", item.second.source->schema()}, {"minRate", 1}, {"maxRate", 120}, {"defaultRate", 60},
    {"policy", "latest"}, {"sampling", "native-thread"}};
  sources["track-controls"] = {{"available",p.controls.available()},{"schema","track-controls.v1"},
    {"minRate",1},{"maxRate",120},{"defaultRate",60},{"policy","latest"},{"sampling","main-thread"}};
  sources["fx-parameters"] = {{"available",p.fxParameters.available()},{"schema","fx-parameters.v1"},
    {"minRate",1},{"maxRate",120},{"defaultRate",60},{"policy","latest"},{"sampling","main-thread"},
    {"chains",Json::array({"main"})},{"containers",p.fxParameters.containersAvailable()},{"maxParameters",32}};
  auto& chains = sources["fx-parameters"]["chains"];
  if (p.fxParameters.inputAvailable()) chains.push_back("input");
  if (p.fxParameters.monitoringAvailable()) chains.push_back("monitoring");
  if (p.fxParameters.takeAvailable()) chains.push_back("take");
  return {{"version", 1}, {"supported", p.scheduler->supported() || p.controls.available() || p.fxParameters.available()}, {"sources", sources},
    {"maxSubscriptions", 32}, {"hardRealtime", false}, {"clock", "host-monotonic-seconds"}};
}
Json RealtimeManager::subscribe(const std::string& name, const Json& options, int window) {
  auto& p = *impl_; p.check(); std::unique_lock<std::mutex> lock(p.mutex);
  if (p.stopped) throw Error("EXTENSION_UNLOADED", "Realtime manager stopped");
  const auto found = p.sources.find(name);
  const bool tracks = name == "track-controls";
  const bool fx = name == "fx-parameters";
  const bool mainThread = tracks || fx;
  if (!mainThread && found == p.sources.end()) throw Error("REALTIME_SOURCE_NOT_FOUND", "Unknown realtime source");
  if (!options.is_object()) throw Error("INVALID_ARGUMENT", "Expected realtime options");
  for (auto i = options.begin(); i != options.end(); ++i)
    if (i.key() != "updateRate" && !(tracks && (i.key() == "tracks" || i.key() == "properties")) &&
        !(fx && (i.key() == "track" || i.key() == "fx" || i.key() == "parameters" || i.key() == "chain" || i.key() == "take")))
      throw Error("INVALID_ARGUMENT", "Unknown realtime option");
  const auto value = options.value("updateRate", Json(60));
  if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < 1 || value.get<double>() > 120)
    throw Error("INVALID_ARGUMENT", "updateRate must be 1..120 Hz");
  if (mainThread ? !p.mainSource(name).available() : (!p.scheduler->supported() || !found->second.source->available()))
    throw Error("REALTIME_UNAVAILABLE", "Realtime source unavailable on this host");
  if (p.subscriptions.size() >= 32) throw Error("QUEUE_LIMIT", "Realtime subscription limit reached");
  if (!mainThread) found->second.source->refresh_context();
  const auto id = ++p.serial;
  Impl::Subscription sub{}; sub.id = id; sub.window = window; sub.rate = value.get<double>(); sub.source = name;
  if (mainThread) {
    lock.unlock();
    sub.tracks = fx ? p.fxParameters.watchFx(options) : p.controls.watch(options);
    lock.lock();
  }
  sub.name = "reaweb.realtime." + name + "." + std::to_string(id);
  ReaWeb_StreamDesc desc{}; desc.size = sizeof(desc); desc.abi_version = REAWEB_STREAM_ABI;
  desc.format = REAWEB_BYTES; desc.max_bytes = fx ? 40 + 32 * 8 : tracks ? TrackControls::maxBytes : 96; desc.capacity = 4; desc.update_rate = sub.rate;
  const auto source = "realtime:" + name; desc.source = source.c_str();
  const auto result = p.streams.create(REAWEB_BINARY, sub.name.c_str(), &desc, &sub.handle);
  if (result) throw Error(StreamHub::code(result), "Cannot create realtime stream");
  p.subscriptions.emplace(id, sub);
  try { p.reschedule(); }
  catch (...) { p.subscriptions.erase(id); p.streams.close(sub.handle); try { p.reschedule(); } catch (...) {} throw; }
  Json descriptor{{"id", id}, {"stream", sub.name}, {"source", name},
    {"schema", fx ? "fx-parameters.v1" : tracks ? "track-controls.v1" : found->second.source->schema()}, {"updateRate", sub.rate}};
  if (tracks) { descriptor["tracks"] = sub.tracks->guids; descriptor["mask"] = sub.tracks->mask; }
  if (fx) {
    descriptor["track"] = sub.tracks->guids.front();
    descriptor["fx"] = sub.tracks->fxGuid;
    descriptor["parameters"] = sub.tracks->parameters;
  }
  return descriptor;
}
void RealtimeManager::unsubscribe(uint64_t id, int window) {
  auto& p = *impl_; p.check(); std::lock_guard<std::mutex> lock(p.mutex); const auto item = p.subscriptions.find(id);
  if (item == p.subscriptions.end()) return;
  if (item->second.window != window) throw Error("INVALID_ARGUMENT", "Realtime subscription belongs to another window");
  const bool tracks = item->second.tracks.has_value();
  const auto source = item->second.source;
  p.streams.close(item->second.handle); p.subscriptions.erase(item); p.reschedule();
  if (tracks) {
    if (std::none_of(p.subscriptions.begin(), p.subscriptions.end(), [&](const auto& s) { return s.second.source == source; }))
      p.mainSource(source).reset();
  } else if (!p.rate) {
    for (auto& source : p.sources) { source.second.source->reset(); source.second.last_sample = 0; }
  }
}
void RealtimeManager::close_window(int window) {
  auto& p = *impl_; p.check(); std::vector<uint64_t> ids;
  { std::lock_guard<std::mutex> lock(p.mutex);
    for (const auto& item : p.subscriptions) if (item.second.window == window) ids.push_back(item.first);
  }
  for (const auto id : ids) unsubscribe(id, window);
}
void RealtimeManager::housekeeping() {
  auto& p = *impl_; p.check(); std::vector<std::pair<uint64_t, int>> expired;
  { std::lock_guard<std::mutex> lock(p.mutex);
    if (p.rate) for (auto& source : p.sources) source.second.source->refresh_context();
    for (auto& item : p.subscriptions) {
      auto& sub = item.second; const auto consumers = p.streams.consumers(sub.handle);
      sub.attached = sub.attached || consumers > 0;
      if (!consumers && (sub.attached || Clock::now() - sub.created > std::chrono::seconds(10))) expired.emplace_back(item.first, sub.window);
    }
  }
  // Track reads stay outside the transport scheduler lock. Only the main thread
  // changes the subscription map or accesses a track subscription's counters.
  bool began = false, fxBegan = false;
  for (auto& entry : p.subscriptions) {
    auto& s = entry.second;
    if (!s.tracks) continue;
    auto& source = p.mainSource(s.source);
    auto& started = s.source == "fx-parameters" ? fxBegan : began;
    if (!started) { source.begin(); started = true; }
    const int reason = source.sample(*s.tracks, s.rate, p.streams, s.handle, s.sequence, s.published, s.dropped);
    if (reason) {
      p.streams.close(s.handle, reason);
      expired.emplace_back(s.id, s.window);
    }
  }
  if (began) p.controls.end();
  if (fxBegan) p.fxParameters.end();
  for (const auto& id : expired) unsubscribe(id.first, id.second);
}
Json RealtimeManager::diagnostics(int window) const {
  auto& p = *impl_; p.check(); std::lock_guard<std::mutex> lock(p.mutex); Json subscriptions = Json::array(), sources = Json::object();
  for (const auto& item : p.subscriptions) {
    const auto& s = item.second; if (s.window != window) continue;
    subscriptions.push_back({{"id", s.id}, {"source", s.source}, {"updateRate", s.rate}, {"published", s.published}, {"publishDropped", s.dropped}});
    if (s.tracks) {
      const auto& w = *s.tracks;
      const double span = seconds(Clock::now()) - w.firstSample;
      subscriptions.back().update(Json{{"sampled",w.samples},{"unchanged",w.unchanged},{"skippedPeriods",w.missed},
        {"sampleHz",w.samples > 1 && span > 0 ? (w.samples - 1) / span : 0},
        {"publishHz",s.published > 1 && span > 0 ? (s.published - 1) / span : 0},
        {"sampleDurationMs",w.durationMs},{"lastPublishedAt",w.lastPublish},
        {"sampleToPublishMs",w.sampleToPublishMs},{"lastTrigger",w.trigger}});
    }
  }
  sources["track-controls"] = p.controls.diagnostics();
  sources["fx-parameters"] = p.fxParameters.diagnostics();
  for (const auto& item : p.sources) {
    sources[item.first] = {{"sampled", item.second.sampled}, {"errors", item.second.errors},
      {"maxSampleIntervalMs", item.second.max_interval * 1000}, {"lastSampleAt", item.second.last_sample}};
  }
  return {{"schedulerRate", p.rate}, {"skippedBusy", p.skipped.load()}, {"subscriptions", subscriptions}, {"sources", sources}};
}
void RealtimeManager::shutdown() {
  auto& p = *impl_; std::lock_guard<std::mutex> lock(p.mutex); if (p.stopped) return;
  p.stopped = true; p.scheduler->stop(); p.rate = 0;
  for (const auto& item : p.subscriptions) p.streams.close(item.second.handle);
  p.subscriptions.clear();
}
}
