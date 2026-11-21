#include "runtime/native_producers.hpp"
#include "runtime/audio_analysis.hpp"
#include "runtime/aggregate_source.hpp"
#include "runtime/native_track_mixer.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace reaweb {
namespace {
template<class F> F function(const Host& host, const char* name) {
  return reinterpret_cast<F>(host.native_function ? host.native_function(name) : nullptr);
}
ReaWeb_StreamDesc audio_desc(unsigned rate = 48000, unsigned channels = 2) {
  ReaWeb_StreamDesc desc{}; desc.size = sizeof(desc); desc.abi_version = 1; desc.format = REAWEB_FLOAT32;
  desc.channels = channels; desc.sample_rate = rate; desc.block_frames = 8192; desc.max_bytes = 8192 * channels * 4; desc.capacity = 16;
  return desc;
}
}
struct NativeProducers::Impl {
  struct TrackSource {
    const Host& host;
    void* track;
    void* project;
    std::string identity;
    unsigned rate, channels = 2;
    double update_rate;
    void* accessor = nullptr;
    std::unique_ptr<AggregateSource> aggregate;
    uint64_t tick = 0, sequence = 0;
    int error = 0, invalid_blocks = 0;
    bool pending = false, discontinuity = false;
    bool meter = false, playing = false, clock_valid = false, reset_on_start = true;
    uint64_t reset_generation = 0, aggregate_generation = 0;
    int meter_frames = 0, revision = 0, layout_revision = 0;
    unsigned layout_channels = 0;
    double last_position = 0;
    std::chrono::steady_clock::time_point last_clock{};
    double position = 0, timestamp = 0;
    std::chrono::steady_clock::time_point next_sample{}, layout_check{};
    std::vector<double> samples = std::vector<double>(8192 * 2);
    std::vector<float> floats = std::vector<float>(8192 * 2);
    TrackSource(const Host& h, void* t, std::string id, unsigned r, double update)
      : host(h), track(t), project(h.current_project ? h.current_project() : nullptr), identity(std::move(id)), rate(r), update_rate(update) {}
    ~TrackSource() { if (accessor) function<void (*)(void*)>(host, "DestroyAudioAccessor")(accessor); }
    int frames() const { return meter ? meter_frames : std::min(8192, std::max(1, static_cast<int>(rate / update_rate))); }
    void meter_sample(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point deadline) {
      char device_rate[64]{};
      auto device = function<bool (*)(const char*, char*, int)>(host, "GetAudioDeviceInfo");
      if (device && device("SRATE", device_rate, sizeof(device_rate)) && std::strtod(device_rate, nullptr) != rate) {
        error = REAWEB_UNSUPPORTED_FORMAT; return;
      }
      auto state = function<int (*)()>(host, "GetPlayState");
      const bool active = state && (state() & 1);
      auto at = function<double (*)()>(host, active ? "GetPlayPosition" : "GetCursorPosition");
      const double current = at ? at() : 0;
      if (!std::isfinite(current)) return;
      const int changed = host.change_count ? host.change_count(project) : 0;
      auto playback_rate = function<double (*)(void*)>(host, "Master_GetPlayRate");
      const double speed = playback_rate ? playback_rate(project) : 1;
      auto refresh = function<bool (*)(void*)>(host, "AudioAccessorValidateState");
      const bool refreshed = accessor && refresh && refresh(accessor);
      const bool jump = clock_valid && (active && playing ?
        current < last_position - 1.0 / rate || std::abs(current - last_position - std::chrono::duration<double>(now - last_clock).count() * speed) > .1 :
        active == playing ? std::abs(current - last_position) > 1.0 / rate :
        active && std::abs(current - last_position) > .1);
      const bool reset = jump || (clock_valid && changed != revision) || refreshed || (active && !playing && reset_on_start);
      if (!clock_valid || reset || active != playing) { position = current; pending = false; }
      if (reset) { ++reset_generation; discontinuity = true; }
      clock_valid = true; playing = active; last_position = current; last_clock = now; revision = changed;
      auto track_value = function<double (*)(void*, const char*)>(host, "GetMediaTrackInfo_Value");
      if (aggregate && (!layout_channels || changed != layout_revision || now >= layout_check)) {
        layout_channels = aggregate->channels(); layout_revision = changed;
        layout_check = now + std::chrono::milliseconds(250);
      }
      const double count = aggregate ? layout_channels : track_value ? track_value(track, "I_NCHAN") : channels;
      if ((count >= 1 ? static_cast<unsigned>(std::min(32., count)) : 2u) != channels) {
        error = REAWEB_UNSUPPORTED_FORMAT; return;
      }
      if (!active || std::chrono::steady_clock::now() >= deadline) return;
      if (!pending) {
        const double available = std::floor((current - position) * rate + 1e-6);
        if (available < 1) return;
        meter_frames = static_cast<int>(std::min(8192.0, available));
      }
      std::fill(samples.begin(), samples.end(), 0);
      auto read = function<int (*)(void*, int, int, double, int, double*)>(host, "GetAudioAccessorSamples");
      const int result = aggregate ? aggregate->read(rate, channels, position, frames(), samples.data(), deadline) :
        read(accessor, rate, channels, position, frames(), samples.data());
      if (aggregate && aggregate_generation != aggregate->generation()) {
        aggregate_generation = aggregate->generation(); ++reset_generation; discontinuity = true;
      }
      pending = aggregate && result == 2;
      if (result < 0) { error = REAWEB_STREAM_CLOSED; return; }
      if (pending) return;
      const auto start = position; position += double(frames()) / rate;
      if (!result) std::fill(samples.begin(), samples.end(), 0);
      if (std::any_of(samples.begin(), samples.begin() + frames() * channels, [](double sample) {
        return !std::isfinite(sample) || std::abs(sample) > std::numeric_limits<float>::max();
      })) {
        discontinuity = true; ++reset_generation;
        if (++invalid_blocks >= 8) error = REAWEB_SERVICE_ERROR;
        return;
      }
      invalid_blocks = 0;
      for (size_t i = 0; i < size_t(frames()) * channels; ++i) floats[i] = static_cast<float>(samples[i]);
      sequence += discontinuity ? 2 : 1; discontinuity = false; timestamp = start;
    }
    void sample(uint64_t cycle, std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point deadline) {
      if (tick == cycle || now < next_sample || error) return;
      tick = cycle;
      auto valid = function<bool (*)(void*, void*, const char*)>(host, "ValidatePtr2");
      if ((host.current_project && host.current_project() != project) || (valid && !valid(project, track, "MediaTrack*"))) {
        error = REAWEB_STREAM_CLOSED; return;
      }
      if (meter) { meter_sample(now, deadline); return; }
      if (aggregate && std::chrono::steady_clock::now() >= deadline) return;
      auto refresh = function<bool (*)(void*)>(host, "AudioAccessorValidateState"); if (accessor && refresh) refresh(accessor);
      auto at = function<double (*)()>(host, "GetPlayPosition"); auto state = function<int (*)()>(host, "GetPlayState");
      if (!state || !(state() & 1)) at = function<double (*)()>(host, "GetCursorPosition");
      if (!pending) position = at ? at() : 0;
      std::fill(samples.begin(), samples.end(), 0);
      auto read = function<int (*)(void*, int, int, double, int, double*)>(host, "GetAudioAccessorSamples");
      const auto result = aggregate ? aggregate->read(rate, 2, position, frames(), samples.data(), deadline) :
        read(accessor, rate, 2, position, frames(), samples.data());
      pending = aggregate && result == 2;
      if (result < 0) error = REAWEB_STREAM_CLOSED;
      else if (!pending) {
        next_sample = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<int64_t>(1000000 / update_rate));
        // Accessors can return a non-finite block around playback boundaries.
        // Preserve the last valid buffer and mark the next block as discontinuous.
        if (std::any_of(samples.begin(), samples.begin() + frames() * 2, [](double sample) {
          return !std::isfinite(sample) || std::abs(sample) > std::numeric_limits<float>::max();
        })) {
          discontinuity = true;
          if (++invalid_blocks >= 8) error = REAWEB_SERVICE_ERROR;
          return;
        }
        invalid_blocks = 0;
        for (int i = 0; i < frames() * 2; ++i) floats[i] = static_cast<float>(samples[i]);
        sequence += discontinuity ? 2 : 1; discontinuity = false; timestamp = position;
      }
    }
  };
  struct Capture {
    // Private PCM envelope: format epochs travel with queued samples, outside the public stream ABI.
    struct Header { unsigned rate, channels; uint64_t generation; uint64_t playing; unsigned stored_channels, reserved; };
    struct Block { Header header{}; std::array<float, 8192 * 32> samples{}; } block;
    StreamBuffer ring{REAWEB_AUDIO, [] { auto desc = audio_desc(48000, 32); desc.format = REAWEB_BYTES; desc.max_bytes += sizeof(Header); return desc; }()};
    std::atomic<unsigned> users{0}, rate{0}, device_channels{2};
    std::atomic<uint64_t> callbacks{0}, unavailable{0};
    std::atomic<int> callback_frames{0}, callback_channels{0};
    std::atomic<bool> has_buffer{false};
    std::atomic<uint64_t> sequence{0};
    uint64_t frames = 0, generation = 0;
    std::chrono::steady_clock::time_point last_capture{};
    double last_duration = 0;
  };
  struct Entry {
    uint64_t handle = 0;
    int window = 0, kind = 0, source = 0, device = -1;
    unsigned rate = 48000, fft_size = 2048, channels = 2;
    double update_rate = 30;
    std::string name, identity;
    void* track = nullptr;
    void* project = nullptr;
#if REAWEB_TRACK_RENDER_CACHE
    Guid live_guid{};
    std::atomic<int> track_index{-1};
#endif
    bool post_fader = false;
    std::atomic<uint64_t> live_sequence{0};
    std::shared_ptr<TrackSource> track_source;
    std::unique_ptr<StreamBuffer> input;
    std::unique_ptr<AudioAnalysis> analysis;
    std::atomic<bool> stopping{false};
    std::atomic<int> error{0};
    std::atomic<bool> attached{false};
    bool force_mono = false, reset_on_start = true;
    std::string integrated_mode;
    std::vector<float> stereo;
    std::atomic<uint64_t> reset_request{1}, reset_after{0};
    uint64_t reset_applied = 0, skip_through = 0, source_reset = 0, format_generation = 0;
    double expected_time = 0;
    bool meter_dirty = true;
    uint64_t sequence = 0, source_sequence = 0, capture_sequence = 0;
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now(), next_output{};
    AudioAnalysis& analyzer() {
      if (!analysis) analysis = std::make_unique<AudioAnalysis>(rate, channels, fft_size, force_mono, kind == REAWEB_METER);
      return *analysis;
    }
    void request_meter_reset(uint64_t after) {
      reset_after = after; ++reset_request;
    }
    void reset_meter_history() {
      if (analysis) analysis->reset();
      capture_sequence = 0; expected_time = 0; next_output = {}; meter_dirty = true;
    }
  };
  const Host& host;
  StreamHub& streams;
  Capture capture[2];
  std::mutex mutex;
  std::vector<std::shared_ptr<Entry>> entries;
  std::atomic<bool> stopping{false}, worker_failed{false};
  std::thread worker;
  uint64_t counter = 0, tick_sequence = 0;
  int last_midi = 0;
  bool midi_initialized = false;
  bool was_playing = false;
  std::atomic<bool> meter_playing{false};
  static constexpr int live_track = 3;
  NativeTrackMixer mixer;
#if REAWEB_TRACK_RENDER_CACHE
  std::atomic<unsigned> live_users{0};
  // The control thread owns one snapshot, the audio thread owns another.
  // Only the middle slot is exchanged. The audio thread never waits for a
  // subscription change or releases the last reference to a retired entry.
  using LiveEntries = std::array<std::shared_ptr<Entry>, 8>;
  std::array<LiveEntries, 3> live_snapshots{};
  static constexpr unsigned snapshot_ready = 4;
  unsigned write_slot = 0, read_slot = 1;
  std::atomic<unsigned> middle_slot{2};
  void update_live_entries() {
    auto& snapshot = live_snapshots[write_slot];
    auto end = std::copy_if(entries.begin(), entries.end(), snapshot.begin(),
        [](const auto& entry) { return entry->source == live_track; });
    std::fill(end, snapshot.end(), nullptr);
    write_slot = middle_slot.exchange(write_slot | snapshot_ready, std::memory_order_acq_rel) & ~snapshot_ready;
  }
  uint64_t audio_cycle = 0;
  using GetTrack = void* (*)(void*, int);
  using GetGuid = const void* (*)(void*);
  using TrackId = int (*)(void*, bool);
  GetTrack get_track = nullptr;
  GetGuid get_guid = nullptr;
  TrackId track_id = nullptr;
  void hardware_tracks(bool post, int frames, double rate) noexcept {
    if (!post) ++audio_cycle;
    if (!live_users.load(std::memory_order_relaxed) || frames < 1 || frames > 8192 ||
        !std::isfinite(rate) || rate < 8000 || rate > 768000) return;
    if (middle_slot.load(std::memory_order_acquire) & snapshot_ready)
      read_slot = middle_slot.exchange(read_slot, std::memory_order_acq_rel) & ~snapshot_ready;
    auto* project = host.current_project();
    std::array<void*, 8> visited{};
    auto end = visited.begin();
    for (const auto& entry : live_snapshots[read_slot]) {
      if (!entry) break;
      if (!entry->attached || entry->stopping || entry->error || entry->project != project ||
          std::find(visited.begin(), end, entry->track) != end) continue;
      const auto index = entry->track_index.load(std::memory_order_relaxed);
      if (index < 0 || get_track(project, index) != entry->track) continue;
      const auto* guid = get_guid(entry->track);
      if (!guid || std::memcmp(entry->live_guid.data(), guid, entry->live_guid.size())) continue;
      *end++ = entry->track;
      mixer.hardware(post, project, entry->track, frames, rate, audio_cycle);
    }
  }
#else
  unsigned live_users = 0;
  void update_live_entries() {}
#endif
  struct LiveBuffer {
    Capture::Header header;
    std::array<float, 8192 * 2> samples;
    std::array<double, 8192 * 2> output;
  };
  std::unique_ptr<LiveBuffer> live;
  static void capture_track(void* context, const NativeTrackBlock& block) noexcept {
    auto& p = *static_cast<Impl*>(context);
#if REAWEB_TRACK_RENDER_CACHE
    const auto& subscriptions = p.live_snapshots[p.read_slot];
#else
    std::unique_lock<std::mutex> lock(p.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const auto& subscriptions = p.entries;
#endif
    for (const auto& entry : subscriptions) {
      if (!entry) break;
      if (entry->source != live_track || entry->track != block.track || !entry->attached || entry->stopping || entry->error) continue;
      if (block.rate != entry->rate) { entry->error = REAWEB_UNSUPPORTED_FORMAT; continue; }
      auto& buffer = *p.live;
      buffer.header = {entry->rate, 2, 1, p.meter_playing.load() ? 1u : 0u, 2, 0};
      auto* samples = block.pcm;
      unsigned stride = block.channels;
      if (entry->post_fader && samples && !block.silence) {
        block.render_output(block, buffer.output.data(), 2);
        samples = buffer.output.data(); stride = 2;
      }
      for (unsigned i = 0; i < block.frames; ++i) for (unsigned ch = 0; ch < 2; ++ch) {
        const double value = samples && !block.silence ? samples[i * stride + std::min(ch, stride - 1)] : 0;
        buffer.samples[2 * i + ch] = std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max() ? static_cast<float>(value) : 0;
      }
      entry->input->publish(&buffer, static_cast<uint32_t>(sizeof(buffer.header) + block.frames * 2 * sizeof(float)),
                            ++entry->live_sequence, block.position);
    }
  }
  explicit Impl(const Host& h, StreamHub& s) : host(h), streams(s), worker([this] { run(); }) {}
  void update_device_channels(int source) {
    // Some hardware hooks leave input_nch/output_nch at zero. Query the device
    // on the main thread and share the same fallback with creation and capture.
    auto count = function<int (*)()>(host, source ? "GetNumAudioOutputs" : "GetNumAudioInputs");
    if (count) capture[source].device_channels = static_cast<unsigned>(std::clamp(count(), 0, 32));
  }
  ~Impl() {
    mixer.stop();
    stopping = true; worker.join();
    for (auto& entry : entries) { entry->track_source.reset(); streams.close(entry->handle); }
  }
  void close(const std::shared_ptr<Entry>& entry) {
    entry->stopping = true;
    entry->track_source.reset();
    if (entry->source == live_track && !--live_users) mixer.active(false);
    if (entry->kind != REAWEB_MIDI && entry->source < 2) --capture[entry->source].users;
    streams.close(entry->handle, entry->error ? entry->error.load() : REAWEB_STREAM_CLOSED);
  }
  void reap() {
    std::vector<std::shared_ptr<Entry>> retired;
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (auto it = entries.begin(); it != entries.end();) {
        auto& entry = **it; const auto users = streams.consumers(entry.handle);
        entry.attached = entry.attached || users > 0;
        if (entry.error || (!users && (entry.attached || now - entry.created > std::chrono::seconds(10)))) {
          entry.stopping = true; retired.push_back(*it); it = entries.erase(it);
        } else ++it;
      }
      if (std::any_of(retired.begin(), retired.end(), [](const auto& entry) { return entry->source == live_track; }))
        update_live_entries();
    }
    for (const auto& entry : retired) close(entry);
  }
  void run() noexcept {
    try {
      while (!stopping) {
        std::vector<std::shared_ptr<Entry>> active;
        { std::lock_guard<std::mutex> lock(mutex); active = entries; }
        for (const auto& entry : active) if (!entry->stopping && !entry->error && entry->kind == REAWEB_METER) {
          try { apply_reset(*entry); } catch (...) { entry->error = REAWEB_SERVICE_ERROR; }
        }
        for (int source = 0; source < 2; ++source) {
          StreamBuffer::Packet packet;
          for (int i = 0; i < 16 && capture[source].ring.consume(packet); ++i)
            for (const auto& entry : active) if (!entry->stopping && entry->source == source && entry->kind != REAWEB_MIDI)
              analyze(*entry, packet);
        }
        for (const auto& entry : active) if (!entry->stopping && entry->input) {
          StreamBuffer::Packet packet; for (int i = 0; i < 16 && entry->input->consume(packet); ++i) analyze(*entry, packet);
        }
        for (const auto& entry : active) if (!entry->stopping && !entry->error && entry->kind == REAWEB_METER && entry->meter_dirty) {
          try { publish_meter(*entry); } catch (...) { entry->error = REAWEB_SERVICE_ERROR; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex); worker_failed = true;
      for (const auto& entry : entries) if (entry->kind != REAWEB_MIDI) entry->error = REAWEB_SERVICE_ERROR;
    }
  }
  void analyze(Entry& entry, const StreamBuffer::Packet& packet) noexcept {
    if (entry.error) return;
    try { analyze_packet(entry, packet); }
    catch (...) { entry.error = REAWEB_SERVICE_ERROR; }
  }
  void analyze_packet(Entry& entry, const StreamBuffer::Packet& packet) {
    if (entry.source < 2 && capture[entry.source].rate.load() != entry.rate) { entry.error = REAWEB_UNSUPPORTED_FORMAT; return; }
    if (entry.kind == REAWEB_METER) { apply_reset(entry); if (packet.sequence <= entry.skip_through) return; }
    const auto* data = packet.data.data(); size_t bytes = packet.data.size();
    bool reset_meter = false, integrate = true;
    unsigned pcm_channels = entry.channels;
    if (entry.source != 2) {
      Capture::Header header; std::memcpy(&header, data, sizeof(header)); data += sizeof(header); bytes -= sizeof(header);
      pcm_channels = header.stored_channels;
      if (entry.kind == REAWEB_METER) {
        if (pcm_channels != entry.channels) { entry.error = REAWEB_UNSUPPORTED_FORMAT; return; }
        if (header.rate != entry.rate) { entry.error = REAWEB_UNSUPPORTED_FORMAT; return; }
        reset_meter = entry.format_generation != header.generation;
        entry.format_generation = header.generation;
        integrate = entry.integrated_mode == "continuous" || header.playing != 0;
      }
    }
    if (entry.kind != REAWEB_METER && pcm_channels != 2) {
      const size_t frames = bytes / (pcm_channels * sizeof(float));
      entry.stereo.resize(frames * 2);
      const auto* pcm = reinterpret_cast<const float*>(data);
      for (size_t i = 0; i < frames; ++i) {
        entry.stereo[i * 2] = pcm[i * pcm_channels];
        entry.stereo[i * 2 + 1] = pcm[i * pcm_channels + (pcm_channels > 1 ? 1 : 0)];
      }
      data = reinterpret_cast<const unsigned char*>(entry.stereo.data()); bytes = frames * 8;
    }
    if (entry.kind == REAWEB_AUDIO) {
      streams.publish(REAWEB_AUDIO, entry.handle, data, static_cast<uint32_t>(bytes), packet.sequence, packet.timestamp); return;
    }
    const bool sequence_gap = entry.capture_sequence && packet.sequence != entry.capture_sequence + 1;
    if (entry.kind == REAWEB_METER) {
      reset_meter = reset_meter || sequence_gap || (entry.capture_sequence && std::abs(packet.timestamp - entry.expected_time) > 1.5 / entry.rate);
      if (reset_meter) entry.reset_meter_history();
    } else if (sequence_gap) entry.analysis.reset();
    entry.capture_sequence = packet.sequence;
    entry.analyzer().process(reinterpret_cast<const float*>(data), bytes / (entry.channels * sizeof(float)), integrate);
    entry.expected_time = packet.timestamp + double(bytes / (entry.channels * sizeof(float))) / entry.rate;
    if (entry.kind == REAWEB_METER) { entry.meter_dirty = true; return; }
    const auto now = std::chrono::steady_clock::now(); if (now < entry.next_output) return;
    entry.next_output = now + std::chrono::microseconds(static_cast<int64_t>(1000000 / entry.update_rate));
    auto result = entry.kind == REAWEB_SPECTRUM ? entry.analysis->spectrum() : entry.analysis->waveform(256);
    streams.publish(entry.kind, entry.handle, result.data(), static_cast<uint32_t>(result.size() * 4), ++entry.sequence, packet.timestamp);
  }
  void apply_reset(Entry& entry) {
    const auto request = entry.reset_request.load();
    if (request == entry.reset_applied) return;
    entry.reset_meter_history();
    entry.skip_through = entry.reset_after.load(); entry.reset_applied = request;
  }
  void publish_meter(Entry& entry) {
    const auto now = std::chrono::steady_clock::now(); if (now < entry.next_output) return;
    const auto result = entry.analyzer().meter();
    streams.publish(entry.kind, entry.handle, result.data(), static_cast<uint32_t>(result.size() * 4), ++entry.sequence, entry.expected_time);
    entry.next_output = now + std::chrono::microseconds(static_cast<int64_t>(1000000 / entry.update_rate));
    entry.meter_dirty = false;
  }
};
NativeProducers::NativeProducers(const Host& host, StreamHub& streams) : impl_(std::make_unique<Impl>(host, streams)) {}
NativeProducers::~NativeProducers() = default;
void NativeProducers::initialize_track_mixer() {
#if REAWEB_TRACK_RENDER_CACHE
  impl_->get_track = function<Impl::GetTrack>(impl_->host, "GetTrack");
  impl_->get_guid = function<Impl::GetGuid>(impl_->host, "GetTrackGUID");
  impl_->track_id = function<Impl::TrackId>(impl_->host, "CSurf_TrackToID");
  if (!impl_->get_track || !impl_->get_guid || !impl_->track_id || !impl_->host.track_guid ||
      !impl_->host.current_project || !impl_->host.valid_track) return;
#endif
  impl_->mixer.initialize(impl_->host, impl_.get(), Impl::capture_track);
}
std::string NativeProducers::audio(const std::string& kind, const Json& options, int window) {
  auto& p = *impl_; auto entry = std::make_shared<Impl::Entry>();
  if (p.worker_failed) throw Error("NATIVE_ERROR", "Audio analysis worker unavailable");
  p.reap();
  { std::lock_guard<std::mutex> lock(p.mutex); if (p.entries.size() >= 8) throw Error("QUEUE_LIMIT", "At most eight built-in producers may be active"); }
  if (!options.is_object()) throw Error("INVALID_ARGUMENT", "Expected audio stream options");
  for (const auto& value : options.items()) if (value.key() != "source" && value.key() != "fftSize" && value.key() != "updateRate" && value.key() != "aggregate" && value.key() != "tap" &&
      !(kind == "meter" && (value.key() == "forceMono" || value.key() == "resetOnPlaybackStart" || value.key() == "integratedMode")))
    throw Error("INVALID_ARGUMENT", "Unknown audio stream option: " + value.key());
  if (!options.value("source", Json("master")).is_string() || !options.value("fftSize", Json(2048)).is_number_integer() ||
      !options.value("updateRate", Json(30)).is_number() || !options.value("aggregate", Json(false)).is_boolean() ||
      !options.value("forceMono", Json(false)).is_boolean() || !options.value("resetOnPlaybackStart", Json(true)).is_boolean())
    throw Error("INVALID_ARGUMENT", "Invalid audio stream options");
  if (options.contains("integratedMode") && (!options["integratedMode"].is_string() ||
      (options["integratedMode"] != "continuous" && options["integratedMode"] != "playback-only")))
    throw Error("INVALID_ARGUMENT", "integratedMode must be continuous or playback-only");
  entry->integrated_mode = options.value("integratedMode", std::string("playback-only"));
  const auto source = options.value("source", std::string("master"));
  const bool aggregate = options.value("aggregate", false);
  std::string tap;
  if (options.contains("tap")) {
    if ((source != "selected-track" && source.rfind("track:", 0) != 0) || !options["tap"].is_string())
      throw Error("INVALID_ARGUMENT", "tap requires a track source");
    tap = options["tap"].get<std::string>();
    if (tap != "pre-fx" && tap != "post-fx" && tap != "post-fader") throw Error("INVALID_ARGUMENT", "Unknown track tap");
    if (aggregate && tap != "pre-fx") throw Error("INVALID_ARGUMENT", "aggregate requires pre-fx");
  }
  if (aggregate && source != "selected-track" && source.rfind("track:", 0) != 0)
    throw Error("INVALID_ARGUMENT", "aggregate requires selected-track or track:<GUID>");
  const auto fft = options.value("fftSize", 2048);
  const auto update = options.value("updateRate", 30.0);
  if (fft < 32 || fft > 32768 || (fft & (fft - 1)) || !std::isfinite(update) || update < 1 || update > 120)
    throw Error("INVALID_ARGUMENT", "FFT size must be a power of two from 32 to 32768; updateRate must be 1..120");
  entry->kind = kind == "audio" ? REAWEB_AUDIO : kind == "spectrum" ? REAWEB_SPECTRUM : kind == "meter" ? REAWEB_METER : kind == "waveform" ? REAWEB_WAVEFORM : 0;
  if (!entry->kind) throw Error("UNSUPPORTED_FORMAT", "Expected audio, spectrum, meter or waveform");
  entry->source = source == "master" ? 1 : source == "input" ? 0 : 2; entry->window = window; entry->fft_size = fft; entry->update_rate = update;
  if (tap == "post-fx" || tap == "post-fader") {
    if (!p.mixer.available()) throw Error("API_UNAVAILABLE", "Native track tap unavailable on this REAPER build");
    entry->source = Impl::live_track;
    entry->post_fader = tap == "post-fader";
  }
  entry->force_mono = options.value("forceMono", false); entry->reset_on_start = options.value("resetOnPlaybackStart", true);
  if (entry->source < 2) {
    p.update_device_channels(entry->source);
    entry->reset_after = p.capture[entry->source].sequence.load();
    if (entry->kind == REAWEB_METER) {
      auto playing = function<int (*)()>(p.host, "GetPlayState");
      p.meter_playing = playing && (playing() & 1);
      const int observed = p.capture[entry->source].callback_channels.load();
      const int channels = observed > 0 ? observed : static_cast<int>(p.capture[entry->source].device_channels.load());
      if (channels < 1) throw Error("AUDIO_UNAVAILABLE", "No active source channels");
      entry->channels = std::min(32, channels);
    }
  }
  const auto device = function<bool (*)(const char*, char*, int)>(p.host, "GetAudioDeviceInfo");
  char rate[64]{};
  if (device && device("SRATE", rate, sizeof(rate))) {
    const double value = std::strtod(rate, nullptr); if (value >= 8000 && value <= 768000) entry->rate = static_cast<unsigned>(value);
  } else if (entry->source != 2) throw Error("AUDIO_UNAVAILABLE", "No active hardware audio device");
  entry->identity = source == "master" ? "hardware-output:0,1" : source == "input" ? "hardware-input:0,1" : source;
  if (entry->source < 2 && entry->kind == REAWEB_METER && entry->channels != 2) {
    entry->identity = entry->source ? "hardware-output:" : "hardware-input:";
    for (unsigned ch = 0; ch < entry->channels; ++ch) entry->identity += (ch ? "," : "") + std::to_string(ch);
  }
  if (entry->source >= 2) {
    if (source == "selected-track") {
      auto selected = function<void* (*)(void*, int)>(p.host, "GetSelectedTrack"); if (selected) entry->track = selected(nullptr, 0);
    } else if (source.rfind("track:", 0) == 0 && p.host.track_count && p.host.track_identity) {
      auto get = function<void* (*)(void*, int)>(p.host, "GetTrack");
      for (int i = 0; get && i < p.host.track_count(); ++i) if (p.host.track_identity(i) == source.substr(6)) { entry->track = get(nullptr, i); break; }
    } else throw Error("INVALID_ARGUMENT", "Source must be master, input, selected-track or track:<GUID>");
    if (!entry->track) throw Error("INVALID_HANDLE", "Audio source track not available");
    if (source == "selected-track" && p.host.track_identity) {
      auto get = function<void* (*)(void*, int)>(p.host, "GetTrack");
      for (int i = 0; get && p.host.track_count && i < p.host.track_count(); ++i)
        if (get(nullptr, i) == entry->track) { entry->identity = "track:" + p.host.track_identity(i); break; }
    }
  }
  if (entry->source == Impl::live_track) {
    entry->identity += ":" + tap;
    entry->project = p.host.current_project ? p.host.current_project() : nullptr;
#if REAWEB_TRACK_RENDER_CACHE
    entry->track_index = p.track_id(entry->track, false) - 1;
    entry->live_guid = p.host.track_guid(entry->track);
#endif
    auto desc = audio_desc(entry->rate, 2);
    desc.format = REAWEB_BYTES;
    desc.max_bytes += sizeof(Impl::Capture::Header);
    entry->input = std::make_unique<StreamBuffer>(REAWEB_AUDIO, desc);
    if (!p.live) p.live = std::make_unique<Impl::LiveBuffer>();
  }
  if (entry->source == 2) {
    auto create = function<void* (*)(void*)>(p.host, "CreateTrackAudioAccessor");
    if (!create || !function<void (*)(void*)>(p.host, "DestroyAudioAccessor") ||
        !function<int (*)(void*, int, int, double, int, double*)>(p.host, "GetAudioAccessorSamples"))
      throw Error("API_UNAVAILABLE", "Track audio accessors unavailable");
    entry->identity += ":pre-fx";
    if (aggregate) entry->identity += ":aggregate-source";
    std::unique_ptr<AggregateSource> meter_aggregate;
    if (entry->kind == REAWEB_METER) {
      if (aggregate) { meter_aggregate = std::make_unique<AggregateSource>(p.host, entry->track); entry->channels = meter_aggregate->channels(); }
      else {
        auto value = function<double (*)(void*, const char*)>(p.host, "GetMediaTrackInfo_Value");
        const double count = value ? value(entry->track, "I_NCHAN") : 2;
        entry->channels = count >= 1 ? static_cast<unsigned>(std::min(32., count)) : 2;
      }
    }
    const auto project = p.host.current_project ? p.host.current_project() : nullptr;
    for (const auto& other : p.entries) {
      const auto& shared = other->track_source;
      if (shared && !shared->error && shared->track == entry->track && shared->project == project &&
          shared->identity == entry->identity && shared->rate == entry->rate && shared->channels == entry->channels && shared->meter == (entry->kind == REAWEB_METER) &&
          (shared->meter ? shared->reset_on_start == entry->reset_on_start : shared->update_rate == update)) {
        entry->track_source = shared; break;
      }
    }
    if (!entry->track_source) {
      entry->track_source = std::make_shared<Impl::TrackSource>(p.host, entry->track, entry->identity, entry->rate, update);
      entry->track_source->meter = entry->kind == REAWEB_METER;
      entry->track_source->channels = entry->channels;
      entry->track_source->samples.resize(8192 * entry->channels);
      entry->track_source->floats.resize(8192 * entry->channels);
      entry->track_source->reset_on_start = entry->reset_on_start;
      if (aggregate) entry->track_source->aggregate = meter_aggregate ? std::move(meter_aggregate) : std::make_unique<AggregateSource>(p.host, entry->track);
      else {
        entry->track_source->accessor = create(entry->track);
        if (!entry->track_source->accessor) throw Error("AUDIO_UNAVAILABLE", "Cannot create track audio accessor");
      }
    }
    entry->input = std::make_unique<StreamBuffer>(REAWEB_AUDIO, audio_desc(entry->rate, entry->channels));
    if (entry->kind == REAWEB_METER) {
      entry->source_sequence = entry->track_source->sequence;
      entry->source_reset = entry->track_source->reset_generation;
      entry->reset_after = entry->source_sequence;
    }
  }
  auto desc = audio_desc(entry->rate, entry->channels); desc.fft_size = fft; desc.update_rate = update; desc.source = entry->identity.c_str();
  if (entry->kind == REAWEB_SPECTRUM) desc.max_bytes = (fft / 2 + 1) * 2 * 4;
  if (entry->kind == REAWEB_METER) desc.max_bytes = static_cast<uint32_t>(AudioAnalysis::meter_size(desc.channels) * 4);
  if (entry->kind == REAWEB_WAVEFORM) desc.max_bytes = std::min(256, fft) * 2 * 2 * 4;
  if (entry->kind != REAWEB_AUDIO) desc.capacity = 3;
  entry->name = "runtime.audio." + std::to_string(++p.counter);
  int status;
  { std::lock_guard<std::mutex> lock(p.mutex); status = p.entries.size() < 8 ? p.streams.create(entry->kind, entry->name.c_str(), &desc, &entry->handle) : REAWEB_QUEUE_LIMIT;
    if (!status) { if (entry->source < 2) ++p.capture[entry->source].users;
      if (p.worker_failed) entry->error = REAWEB_SERVICE_ERROR;
      p.entries.push_back(entry);
      if (entry->source == Impl::live_track) {
        p.update_live_entries();
        if (++p.live_users == 1) p.mixer.active(true);
      } } }
  if (status) throw Error(StreamHub::code(status), "Cannot create audio analysis stream");
  return entry->name;
}
void NativeProducers::reset_meter(const std::string& name, int window) {
  auto& p = *impl_; std::lock_guard<std::mutex> lock(p.mutex);
  for (const auto& entry : p.entries) if (entry->name == name && entry->kind == REAWEB_METER && entry->window == window && !entry->stopping && !entry->error) {
    entry->request_meter_reset(entry->source < 2 ? p.capture[entry->source].sequence.load() :
      entry->source == Impl::live_track ? entry->live_sequence.load() : entry->track_source->sequence);
    return;
  }
  throw Error("INVALID_HANDLE", "Expected a built-in meter opened by this window");
}
std::string NativeProducers::midi(int device, int window) {
  auto& p = *impl_;
  if (device < -1 || device > 65535) throw Error("INVALID_ARGUMENT", "Expected -1 for all MIDI inputs or a device index");
  if (!function<int (*)(int, char*, int*, int*, int*, double*, int*)>(p.host, "MIDI_GetRecentInputEvent")) throw Error("API_UNAVAILABLE", "MIDI input history unavailable");
  auto entry = std::make_shared<Impl::Entry>(); entry->kind = REAWEB_MIDI; entry->window = window; entry->device = device;
  entry->name = "runtime.midi." + std::to_string(++p.counter); entry->identity = "midi-input:" + std::to_string(device);
  ReaWeb_StreamDesc desc{}; desc.size = sizeof(desc); desc.abi_version = 1; desc.format = REAWEB_BYTES; desc.max_bytes = 4096; desc.capacity = 64; desc.source = entry->identity.c_str();
  std::lock_guard<std::mutex> lock(p.mutex);
  if (p.entries.size() >= 8) throw Error("QUEUE_LIMIT", "At most eight built-in producers may be active");
  const auto status = p.streams.create(REAWEB_MIDI, entry->name.c_str(), &desc, &entry->handle);
  if (status) throw Error(StreamHub::code(status), "Cannot create MIDI stream");
  p.entries.push_back(entry); return entry->name;
}
void NativeProducers::capture(bool output, int frames, double rate, int channels, double* (*get)(bool, int)) noexcept {
#if REAWEB_TRACK_RENDER_CACHE
  impl_->hardware_tracks(output, frames, rate);
#endif
  auto& capture = impl_->capture[output ? 1 : 0];
  ++capture.callbacks;
  capture.callback_frames = frames; capture.callback_channels = channels; capture.has_buffer = get != nullptr;
  if (!capture.users.load(std::memory_order_relaxed) || !get || frames < 1 || frames > 8192 || !std::isfinite(rate) || rate < 8000 || rate > 768000) { ++capture.generation; return; }
  const auto now = std::chrono::steady_clock::now();
  if (capture.last_capture.time_since_epoch().count() &&
      std::chrono::duration<double>(now - capture.last_capture).count() > std::max(.1, capture.last_duration * 4)) ++capture.generation;
  capture.last_capture = now; capture.last_duration = frames / rate;
  if (capture.block.header.rate != static_cast<unsigned>(rate) || capture.block.header.channels != static_cast<unsigned>(channels)) ++capture.generation;
  capture.rate.store(static_cast<unsigned>(rate), std::memory_order_relaxed);
  const unsigned stored = channels > 0 ? std::min(32, channels) : capture.device_channels.load(std::memory_order_relaxed);
  if (!stored) { ++capture.unavailable; ++capture.generation; return; }
  std::array<double*, 32> buffers{};
  for (unsigned ch = 0; ch < stored; ++ch) buffers[ch] = get(output, ch);
  if (!buffers[0]) { ++capture.unavailable; ++capture.generation; return; }
  if (stored > 1 && (channels <= 0 || stored == 2) && !buffers[1]) buffers[1] = buffers[0];
  capture.block.header = {static_cast<unsigned>(rate), static_cast<unsigned>(channels), capture.generation, impl_->meter_playing.load() ? 1u : 0u, stored, 0};
  for (int i = 0; i < frames; ++i) for (unsigned ch = 0; ch < stored; ++ch)
    capture.block.samples[i * stored + ch] = buffers[ch] ? static_cast<float>(buffers[ch][i]) : 0;
  capture.ring.publish(&capture.block, static_cast<uint32_t>(sizeof(Impl::Capture::Header) + frames * stored * 4), ++capture.sequence, capture.frames / rate);
  capture.frames += frames;
}
void NativeProducers::attached(const std::string& name) {
  auto& p = *impl_; std::lock_guard<std::mutex> lock(p.mutex);
  for (const auto& entry : p.entries) if (entry->name == name) entry->attached = true;
}
void NativeProducers::tick() {
  auto& p = *impl_; p.reap();
  for (int source = 0; source < 2; ++source) if (p.capture[source].users.load()) p.update_device_channels(source);
  std::vector<std::shared_ptr<Impl::Entry>> active;
  { std::lock_guard<std::mutex> lock(p.mutex); active = p.entries; }
  const auto now = std::chrono::steady_clock::now();
  const auto deadline = now + std::chrono::milliseconds(2);
  ++p.tick_sequence;
  if (std::any_of(active.begin(), active.end(), [](const auto& entry) { return entry->kind == REAWEB_METER && entry->source != 2; })) {
    auto play_state = function<int (*)()>(p.host, "GetPlayState");
    const bool playing = play_state && (play_state() & 1);
    p.meter_playing = playing;
    for (const auto& entry : active) if (entry->kind == REAWEB_METER && entry->source != 2 && entry->reset_on_start && playing && !p.was_playing) {
      entry->request_meter_reset(entry->source == Impl::live_track ? entry->live_sequence.load() : p.capture[entry->source].sequence.load());
    }
    p.was_playing = playing;
  }
  for (size_t i = 0; i < active.size(); ++i) {
    auto& entry = *active[(i + p.tick_sequence - 1) % active.size()];
    if (entry.source == Impl::live_track && ((p.host.current_project && p.host.current_project() != entry.project) ||
        (p.host.valid_track && !p.host.valid_track(entry.project, entry.track)))) entry.error = REAWEB_STREAM_CLOSED;
#if REAWEB_TRACK_RENDER_CACHE
    if (entry.source == Impl::live_track && !entry.error) {
      if (p.host.track_guid(entry.track) != entry.live_guid) entry.error = REAWEB_STREAM_CLOSED;
      else entry.track_index = p.track_id(entry.track, false) - 1;
    }
#endif
    if (!entry.attached || !entry.track_source) continue;
    auto& source = *entry.track_source;
    source.sample(p.tick_sequence, now, deadline);
    if (entry.kind == REAWEB_METER && entry.source_reset != source.reset_generation) {
      entry.source_reset = source.reset_generation; entry.request_meter_reset(entry.source_sequence);
    }
    if (source.error) entry.error = source.error;
    else if (entry.source_sequence != source.sequence) {
      entry.source_sequence = source.sequence;
      entry.input->publish(source.floats.data(), source.frames() * source.channels * sizeof(float), entry.source_sequence, source.timestamp);
    }
  }
  if (std::none_of(p.entries.begin(), p.entries.end(), [](const auto& e) { return e->kind == REAWEB_MIDI; })) { p.midi_initialized = false; return; }
  auto recent = function<int (*)(int, char*, int*, int*, int*, double*, int*)>(p.host, "MIDI_GetRecentInputEvent");
  struct MIDI { int sequence, timestamp, device; double position; std::vector<unsigned char> data; };
  std::vector<MIDI> messages;
  for (int index = 0; index < 256; ++index) {
    char buffer[1024]; int size = sizeof(buffer), stamp = 0, device = 0, loop = 0; double position = 0;
    const auto sequence = recent(index, buffer, &size, &stamp, &device, &position, &loop);
    if (!sequence || sequence == p.last_midi) break;
    if (!p.midi_initialized) { p.last_midi = sequence; p.midi_initialized = true; break; }
    if (size > 0 && size <= int(sizeof(buffer))) messages.push_back({sequence, stamp, device, position, {buffer, buffer + size}});
  }
  p.midi_initialized = true;
  if (messages.empty()) return;
  p.last_midi = messages.front().sequence;
  for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
    std::vector<unsigned char> bytes(16 + it->data.size());
    const auto put = [&](size_t at, uint32_t value) { for (int i = 0; i < 4; ++i) bytes[at + i] = static_cast<unsigned char>(value >> (i * 8)); };
    put(0, it->device); put(4, it->timestamp); put(8, static_cast<uint32_t>(it->data.size())); put(12, 0);
    std::copy(it->data.begin(), it->data.end(), bytes.begin() + 16);
    for (const auto& entry : p.entries) if (entry->kind == REAWEB_MIDI && (entry->device == -1 || entry->device == (it->device & 65535)))
      p.streams.publish(REAWEB_MIDI, entry->handle, bytes.data(), static_cast<uint32_t>(bytes.size()), static_cast<uint32_t>(it->sequence), it->position);
  }
}
void NativeProducers::close_window(int window) {
  auto& p = *impl_; std::lock_guard<std::mutex> lock(p.mutex);
  bool removed_live = false;
  for (auto it = p.entries.begin(); it != p.entries.end();) {
    if ((*it)->window != window) { ++it; continue; }
    if (p.streams.consumers((*it)->handle)) { (*it)->window = 0; ++it; }
    else {
      removed_live |= (*it)->source == Impl::live_track;
      p.close(*it); it = p.entries.erase(it);
    }
  }
  if (removed_live) p.update_live_entries();
}
Json NativeProducers::devices() const {
  const auto& host = impl_->host; Json result{{"audio", Json::object()}, {"midiInputs", Json::array()}, {"midiOutputs", Json::array()}};
  auto audio = function<bool (*)(const char*, char*, int)>(host, "GetAudioDeviceInfo");
  for (const auto* key : {"MODE", "IDENT_IN", "IDENT_OUT", "BSIZE", "SRATE", "BPS"}) {
    char value[1024]{}; result["audio"][key] = audio && audio(key, value, sizeof(value)) ? Json(value) : Json();
  }
  for (const bool input : {true, false}) {
    auto channel_count = function<int (*)()>(host, input ? "GetNumAudioInputs" : "GetNumAudioOutputs");
    auto channel_name = function<const char* (*)(int)>(host, input ? "GetInputChannelName" : "GetOutputChannelName");
    auto& channels = result["audio"][input ? "inputs" : "outputs"]; channels = Json::array();
    for (int i = 0, total = channel_count ? std::min(256, channel_count()) : 0; i < total; ++i) {
      const auto* value = channel_name ? channel_name(i) : nullptr;
      channels.push_back({{"id", i}, {"name", value ? value : ""}});
    }
    auto count = function<int (*)()>(host, input ? "GetNumMIDIInputs" : "GetNumMIDIOutputs");
    auto name = function<bool (*)(int, char*, int)>(host, input ? "GetMIDIInputName" : "GetMIDIOutputName");
    if (!count || !name) continue;
    for (int i = 0, total = std::min(256, count()); i < total; ++i) { char text[1024]{}; const bool present = name(i, text, sizeof(text));
      if (!present && !*text) continue;
      result[input ? "midiInputs" : "midiOutputs"].push_back({{"id", i}, {"name", text}, {"present", present}}); }
  }
  return result;
}
Json NativeProducers::diagnostics() const {
  Json result = Json::array();
  for (const auto& source : impl_->capture) result.push_back({{"callbacks", source.callbacks.load()}, {"users", source.users.load()},
    {"sampleRate", source.rate.load()}, {"published", source.ring.published.load()}, {"dropped", source.ring.dropped.load()}, {"unavailable", source.unavailable.load()},
    {"blockFrames", source.callback_frames.load()}, {"channels", source.callback_channels.load()}, {"bufferAvailable", source.has_buffer.load()}});
  return result;
}
}
