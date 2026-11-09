#include "runtime/video.hpp"
#include "video/pixels.hpp"
#include <algorithm>
#include <cmath>

namespace reaweb {
namespace {
using Clock = std::chrono::steady_clock;
template<class F> F function(const Host& host, const char* name) {
  return reinterpret_cast<F>(host.native_function ? host.native_function(name) : nullptr);
}
double seconds() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
int option(const Json& options, const char* name, int fallback, int maximum) {
  const auto v = options.value(name, Json(fallback));
  if (!v.is_number_integer() || v.get<double>() < 1 || v.get<double>() > maximum)
    throw Error("INVALID_ARGUMENT", std::string(name) + " is outside the supported video range");
  return v.get<int>();
}
}
struct VideoManager::Impl {
  const Host& host;
  StreamHub& streams;
  std::unique_ptr<VideoBackend> backend;
  std::unique_ptr<VideoScheduler> scheduler;
  std::function<void()> wake;
  bool stopped = false, had_consumers = false, valid = false, discontinuity = true;
  void* project = nullptr;
  uint64_t generation = 0, epoch = 1, serial = 0, handle = 0, sequence = 0;
  uint64_t produced = 0, dropped = 0, failed = 0, captured = 0, cleared = 0;
  std::string name, error;
  int width = 1280, height = 720, source_width = 0, source_height = 0;
  double rate = 0, target_rate = 30;
  int previous_changes = -1, previous_play = -1;
  std::vector<unsigned char> pixels;
  double idle_at = 0, next_capture = 0, first_capture = 0, last_capture = 0, pending_since = 0;
  double position = 0, source_rate = 0, capture_ms = 0, previous_position = 0, previous_tick = 0;
  Impl(const Host& h, StreamHub& s, std::unique_ptr<VideoBackend> b)
    : host(h), streams(s), backend(b ? std::move(b) : make_video_backend(h)) {}

  Json requested_rate() const { return rate ? Json(rate) : Json("auto"); }
  double project_frame_rate() const {
    const auto fps = function<double(*)(void*, bool*)>(host, "TimeMap_curFrameRate");
    const double value = fps ? fps(project, nullptr) : 0;
    return std::isfinite(value) && value > 0 ? value : 0;
  }
  double resolve_rate(double fps) const { return rate ? rate : fps > 0 ? std::clamp(fps, 1.0, 75.0) : 30.0; }

  void publish(double time, bool reset = false) {
    if (streams.publish(REAWEB_FRAME, handle, pixels.data(), uint32_t(pixels.size()), ++sequence, time, nullptr, reset)) ++dropped;
    else ++produced;
  }
  void clear(double time, const char* reason) {
    if (valid || !sequence) {
      std::fill(pixels.begin(), pixels.end(), 0);
      for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
      publish(time, true); ++cleared;
    }
    valid = false; source_width = source_height = 0; last_capture = pending_since = 0; error = reason;
  }
  void close() {
    if (scheduler) scheduler->stop();
    backend->stop();
    if (handle) streams.close(handle);
    handle = 0; name.clear(); std::vector<unsigned char>().swap(pixels);
    project = nullptr; valid = false; source_width = source_height = 0; last_capture = pending_since = 0;
  }
  bool capture(const VideoImage& image, double time, double fps) {
    const int w = image.width, h = image.height;
    if (!image.pixels || w <= 0 || h <= 0 || w > 16384 || h > 16384 || image.stride < int64_t(w) * 4) return false;
    copy_video_pixels(image, width, height, pixels);
    publish(time); ++captured;
    if (!first_capture) first_capture = seconds();
    source_width = w; source_height = h; position = time; source_rate = fps;
    last_capture = seconds(); valid = true; error.clear(); return true;
  }
};
VideoManager::VideoManager(const Host& h, StreamHub& s, std::unique_ptr<VideoBackend> backend,
    std::unique_ptr<VideoScheduler> scheduler, std::function<void()> wake)
  : impl_(new Impl(h, s, std::move(backend))) {
  impl_->scheduler = std::move(scheduler);
  impl_->wake = wake ? std::move(wake) : std::function<void()>([this] { tick(); });
}
VideoManager::~VideoManager() { shutdown(); }
Json VideoManager::capabilities() const {
  const auto& s = *impl_;
  const bool available = !s.stopped && s.backend->supported();
#ifdef _WIN32
  const char* platform = "windows";
#elif defined(__APPLE__)
  const char* platform = "macos";
#else
  const char* platform = "linux";
#endif
  return {{"available", available}, {"backendAvailable", available}, {"companionInstalled", false}, {"platform", platform},
    {"backend", "reaper-internal"}, {"source", "project"},
    {"requiresVideoWindow", false}, {"requiresCompanion", false}, {"companion", nullptr},
    {"hiddenVideoWindow", true}, {"maxWidth", 1920}, {"maxHeight", 1080}, {"maxUpdateRate", 75},
    {"defaultWidth", 1280}, {"defaultHeight", 720}, {"defaultUpdateRate", "auto"}, {"autoUpdateRate", true},
    {"profiles", 1}, {"profilePolicy", "first-consumer"}, {"format", "rgba8"},
    {"error", available ? Json(nullptr) : Json("VIDEO_HOST_UNSUPPORTED")}};
}
std::string VideoManager::open(const Json& options, int window) {
  auto& s = *impl_;
  if (!options.is_object() || window <= 0) throw Error("INVALID_ARGUMENT", "Expected video stream options and an App window");
  for (const auto& item : options.items()) if (item.key() != "source" && item.key() != "width" && item.key() != "height" && item.key() != "updateRate")
    throw Error("INVALID_ARGUMENT", "Unknown video option: " + item.key());
  if (options.value("source", Json("project")) != "project") throw Error("INVALID_ARGUMENT", "Only the current project video source is supported");
  const int w = option(options, "width", 1280, 1920), h = option(options, "height", 720, 1080);
  const auto requested_rate = options.value("updateRate", Json("auto"));
  double rate = 0;
  if (requested_rate != "auto") {
    if (!requested_rate.is_number()) throw Error("INVALID_ARGUMENT", "updateRate must be auto or a number from 1 to 75");
    rate = requested_rate.get<double>();
    if (!std::isfinite(rate) || rate < 1 || rate > 75)
      throw Error("INVALID_ARGUMENT", "updateRate is outside the supported video range");
  }
  if (s.stopped || !s.backend->supported()) throw Error("VIDEO_HOST_UNSUPPORTED", "The REAPER video backend is unavailable on this host");
  if (s.handle) {
    if (w != s.width || h != s.height || rate != s.rate) throw Error("VIDEO_PROFILE_CONFLICT", "Active project video consumers share one resolution and update rate", {{"width", s.width}, {"height", s.height}, {"updateRate", s.requested_rate()}});
    s.idle_at = seconds(); return s.name;
  }
  s.width = w; s.height = h; s.rate = rate;
  s.project = s.host.current_project ? s.host.current_project() : nullptr;
  s.generation = s.host.project_generation ? s.host.project_generation() : 0;
  s.target_rate = s.resolve_rate(s.project_frame_rate());
  try {
    s.pixels.resize(size_t(w) * h * 4);
    s.name = "reaweb.video.project." + std::to_string(++s.serial);
    ReaWeb_StreamDesc desc{};
    desc.size = sizeof(desc); desc.abi_version = REAWEB_STREAM_ABI; desc.format = REAWEB_PIXEL_RGBA8;
    desc.max_bytes = uint32_t(s.pixels.size()); desc.capacity = 3; desc.width = w; desc.height = h; desc.stride = w * 4;
    desc.update_rate = s.target_rate; desc.source = "project";
    const int result = s.streams.create(REAWEB_FRAME, s.name.c_str(), &desc, &s.handle, true);
    if (result) throw Error(StreamHub::code(result), "Cannot create project video stream");
    s.next_capture = s.first_capture = s.last_capture = s.previous_tick = 0;
    s.produced = s.dropped = s.failed = s.captured = s.sequence = s.cleared = 0;
    s.source_rate = 0; s.previous_changes = s.previous_play = -1;
    s.valid = false; s.discontinuity = true; s.idle_at = seconds(); s.had_consumers = false;
    s.clear(0, "VIDEO_FRAME_PENDING");
    if (s.scheduler) s.scheduler->start(s.wake);
    return s.name;
  } catch (...) { s.close(); throw; }
}
void VideoManager::tick() {
  auto& s = *impl_;
  if (!s.handle) return;
  const auto project = s.host.current_project ? s.host.current_project() : nullptr;
  const auto generation = s.host.project_generation ? s.host.project_generation() : 0;
  if (project != s.project || generation != s.generation) { ++s.epoch; s.error = "VIDEO_PROJECT_CHANGED"; s.close(); return; }
  if (!s.streams.consumers(s.handle)) {
    s.backend->stop();
    if (s.had_consumers || seconds() - s.idle_at > 10) s.close();
    return;
  }
  const double now = seconds(); s.idle_at = now; s.had_consumers = true;
  const auto play = function<int(*)()>(s.host, "GetPlayState");
  const int playing = play ? play() : 0;
  const auto clock = function<double(*)()>(s.host, (playing & 3) ? "GetPlayPosition" : "GetCursorPosition");
  const double position = clock ? clock() : 0;
  const auto rate = function<double(*)(void*)>(s.host, "Master_GetPlayRate");
  const double play_rate = rate ? rate(project) : 1;
  const int changes = s.host.change_count ? s.host.change_count(project) : 0;
  const double expected = s.previous_position + ((s.previous_play & 1) ? (now - s.previous_tick) * play_rate : 0);
  if (changes != s.previous_changes || playing != s.previous_play ||
      (s.previous_tick && std::abs(position - expected) > ((playing & 1) ? 0.15 : 0.000001))) {
    s.discontinuity = true; s.clear(position, "VIDEO_FRAME_PENDING");
  }
  s.previous_position = position; s.previous_tick = now; s.previous_play = playing; s.previous_changes = changes;
  const double source_rate = s.project_frame_rate();
  const double target_rate = s.resolve_rate(source_rate);
  if (target_rate != s.target_rate) { s.target_rate = target_rate; s.next_capture = now; s.discontinuity = true; }
  // Auto follows distinct host frames instead of a second, unsynchronised clock.
  const bool follow_frames = !s.rate && (playing & 1) && source_rate > 0 && source_rate <= 75 &&
    std::isfinite(play_rate) && play_rate > 0 && play_rate <= 1;
  if (!follow_frames && now + 0.5 / s.target_rate < s.next_capture) return;
  bool copied = false;
  try {
    const auto result = s.backend->read(s.discontinuity, [&](const VideoImage& image) {
      if (std::isfinite(position)) copied = s.capture(image, position, source_rate);
    });
    s.discontinuity = false;
    if (result != VideoRead::pending) s.pending_since = 0;
    if (result == VideoRead::unsupported) { s.error = "VIDEO_HOST_UNSUPPORTED"; s.close(); return; }
    if (result == VideoRead::unchanged || result == VideoRead::pending) {
      s.capture_ms = (seconds() - now) * 1000;
      if (result == VideoRead::unchanged && !(playing & 1)) s.next_capture = now + 1.0 / s.target_rate;
      if (result == VideoRead::pending) {
        if (!s.pending_since) s.pending_since = now;
        if (!s.valid || now - s.pending_since > 0.1) {
          s.clear(position, "VIDEO_FRAME_PENDING"); s.discontinuity = true;
        }
      }
      return;
    }
    s.next_capture = follow_frames ? now + 1.0 / s.target_rate : std::max(s.next_capture + 1.0 / s.target_rate, now);
    if (!copied) {
      s.discontinuity = true;
      if (result == VideoRead::ready) ++s.failed;
      s.clear(position, result == VideoRead::rendering ? "VIDEO_RENDER_BUSY" :
        result == VideoRead::empty || result == VideoRead::ready ? "VIDEO_SOURCE_UNAVAILABLE" : "VIDEO_FRAME_PENDING");
    }
  } catch (...) { ++s.failed; s.backend->stop(); s.clear(position, "VIDEO_SOURCE_UNAVAILABLE"); s.discontinuity = true; }
  s.capture_ms = (seconds() - now) * 1000;
}
void VideoManager::close_window(int window) { impl_->streams.detach_stream_window(impl_->handle, window); }
Json VideoManager::state() const {
  const auto& s = *impl_;
  const auto toggle = function<int(*)(int)>(s.host, "GetToggleCommandState");
  const bool visible = toggle && toggle(50125) > 0;
  return {{"source", "project"}, {"backend", "reaper-internal"},
    {"status", !s.handle ? "closed" : !s.had_consumers ? "idle" : s.valid ? "ready" : "waiting"}, {"epoch", s.epoch},
    {"width", s.width}, {"height", s.height}, {"sourceWidth", s.source_width}, {"sourceHeight", s.source_height},
    {"updateRate", s.requested_rate()}, {"targetUpdateRate", s.target_rate},
    {"position", s.valid ? Json(s.position) : Json(nullptr)}, {"sourceFrameRate", s.source_rate},
    {"frameAgeMs", s.last_capture ? Json((seconds() - s.last_capture) * 1000) : Json(nullptr)}, {"videoWindowOpen", visible},
    {"subscribers", s.handle ? s.streams.consumers(s.handle) : 0}, {"stream", s.name},
    {"error", s.error.empty() ? Json(nullptr) : Json(s.error)}};
}
Json VideoManager::diagnostics() const {
  auto result = state(); const auto& s = *impl_;
  const double elapsed = s.first_capture ? seconds() - s.first_capture : 0;
  const double latency = s.streams.frame_ack_ms(s.handle);
  result.update({{"producedFrames", s.produced}, {"capturedFrames", s.captured}, {"clearedFrames", s.cleared},
    {"droppedFrames", s.dropped}, {"captureFailures", s.failed},
    {"actualFPS", elapsed > 0 && s.captured ? (s.captured - 1) / elapsed : 0}, {"captureMs", s.capture_ms},
    {"transportLatencyMs", latency >= 0 ? Json(latency) : Json(nullptr)}, {"transportDiagnostics", "reaper.stream.getDiagnostics"}});
  return result;
}
void VideoManager::shutdown() {
  auto& s = *impl_; if (s.stopped) return;
  s.close(); s.stopped = true;
}
}
