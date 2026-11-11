#include "runtime/video.hpp"
#include "video/pixels.hpp"
#include "video/position_clock.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

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
Json color_conversion(VideoColorConversion value) {
  return value == VideoColorConversion::reaper_neon ? Json("reaper-neon") :
    value == VideoColorConversion::reaper_sse ? Json("reaper-sse") : Json();
}
Json color_matrix(int matrix) {
  return matrix == 0 ? Json("bt601") : matrix == 1 ? Json("bt709") : matrix == 2 ? Json("bt2020") : Json();
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
  std::string name, error, resolution = "custom";
  std::string format = "rgba8";
  bool native_i420 = false;
  int matrix = -1;
  VideoColorConversion conversion = VideoColorConversion::unknown;
  bool preserve_aspect = true, probing = false;
  double probe_started = 0;
  int resolved_source_width = 0, resolved_source_height = 0;
  int width = 1280, height = 720, source_width = 0, source_height = 0;
  double rate = 0, target_rate = 30;
  std::vector<unsigned char> pixels;
  double idle_at = 0, next_capture = 0, first_capture = 0, last_capture = 0, pending_since = 0;
  double position = 0, source_rate = 0, capture_ms = 0;
  VideoPositionClock position_clock;
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
    if (streams.publish(REAWEB_FRAME, handle, pixels.data(), uint32_t(pixels.size()), ++sequence, time, nullptr, reset, &pixels)) ++dropped;
    else ++produced;
  }
  void clear(double time, const char* reason) {
    if (valid || !sequence) {
      std::fill(pixels.begin(), pixels.end(), 0);
      if (format == "i420") {
        std::fill(pixels.begin(), pixels.begin() + size_t(width) * height, 16);
        std::fill(pixels.begin() + size_t(width) * height, pixels.end(), 128);
      } else for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
      publish(time, true); ++cleared;
    }
    valid = false; source_width = source_height = 0; last_capture = pending_since = 0; error = reason;
  }
  void close() {
    if (scheduler) scheduler->stop();
    probing = false;
    backend->stop();
    if (handle) streams.close(handle);
    handle = 0; name.clear(); std::vector<unsigned char>().swap(pixels);
    project = nullptr; valid = false; source_width = source_height = 0; last_capture = pending_since = 0;
  }
  bool capture(const VideoImage& image, double time, double fps) {
    const int w = image.width, h = image.height;
    if (!image.pixels || w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    if (format == "i420") {
      if (image.format != VideoPixelFormat::i420 || image.color_matrix != 1 || image.color_conversion != conversion || w != width || h != height ||
          (w & 1) || (h & 1) || (image.stride & 1) || image.stride < w || image.stride > 262144) return false;
      if (image.stride == w) std::memcpy(pixels.data(), image.pixels, pixels.size());
      else {
        const auto* source = image.pixels; auto* target = pixels.data();
        for (int plane = 0; plane < 3; ++plane) {
          const int rows = plane ? h / 2 : h, columns = plane ? w / 2 : w, stride = plane ? image.stride / 2 : image.stride;
          for (int row = 0; row < rows; ++row) std::memcpy(target + size_t(row) * columns, source + size_t(row) * stride, columns);
          source += size_t(rows) * stride; target += size_t(rows) * columns;
        }
      }
    } else {
      if (image.format != VideoPixelFormat::bgra8 || image.stride < int64_t(w) * 4) return false;
      copy_video_pixels(image, width, height, pixels, resolution == "custom" && preserve_aspect);
    }
    publish(time); ++captured;
    if (!first_capture) first_capture = seconds();
    source_width = w; source_height = h; position = time; source_rate = fps;
    native_i420 = image.native_i420; matrix = image.color_matrix; conversion = image.color_conversion;
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
    {"hiddenVideoWindow", true}, {"maxWidth", 8192}, {"maxHeight", 8192}, {"maxFrameBytes", video_max_frame_bytes},
    {"maxPixels", video_max_frame_bytes / 4}, {"resolutionModes", Json::array({"custom", "fit", "source"})}, {"maxUpdateRate", 75},
    {"defaultWidth", 1280}, {"defaultHeight", 720}, {"defaultUpdateRate", "auto"}, {"autoUpdateRate", true},
    {"profiles", 1}, {"profilePolicy", "first-consumer"}, {"format", "rgba8"},
    {"formats", s.backend->supports_i420() ? Json::array({"rgba8", "i420"}) : Json::array({"rgba8"})},
    {"error", available ? Json(nullptr) : Json("VIDEO_HOST_UNSUPPORTED")}};
}
Json VideoManager::source_info() {
  auto& s = *impl_;
  int width = 0, height = 0;
  bool native_i420 = false;
  int matrix = -1;
  auto conversion = VideoColorConversion::unknown;
  std::string error;
  if (s.handle) {
    tick();
    if (s.valid) { width = s.source_width; height = s.source_height; native_i420 = s.native_i420; matrix = s.matrix; conversion = s.conversion; }
    else error = s.error.empty() ? "VIDEO_FRAME_PENDING" : s.error;
  } else if (s.stopped || !s.backend->supported()) error = "VIDEO_HOST_UNSUPPORTED";
  else {
    try {
      const auto result = s.backend->read(true, [&](const VideoImage& image) {
        if (image.pixels && image.width > 0 && image.height > 0 && image.stride >= int64_t(image.width) * 4) {
          width = image.width; height = image.height;
          native_i420 = image.native_i420; matrix = image.color_matrix; conversion = image.color_conversion;
        }
      });
      if (!width) error = result == VideoRead::rendering ? "VIDEO_RENDER_BUSY" :
        result == VideoRead::empty ? "VIDEO_SOURCE_UNAVAILABLE" : "VIDEO_FRAME_PENDING";
    } catch (...) { error = "VIDEO_SOURCE_UNAVAILABLE"; }
    if (!width && error == "VIDEO_FRAME_PENDING") {
      if (!s.probing) {
        s.probing = true; s.probe_started = seconds();
        if (s.scheduler) s.scheduler->start(s.wake);
      }
    } else {
      if (s.probing && s.scheduler) s.scheduler->stop();
      s.probing = false; s.backend->stop();
    }
  }
  const bool supported = width > 0 && width <= 8192 && height <= 8192 && uint64_t(width) * height * 4 <= video_max_frame_bytes;
  return {{"available", width > 0}, {"width", width}, {"height", height}, {"originalSupported", supported},
    {"nativeFormat", native_i420 ? Json("i420") : Json()}, {"colorMatrix", native_i420 ? color_matrix(matrix) : Json()},
    {"colorRange", native_i420 && matrix >= 0 && matrix <= 2 ? Json("limited") : Json()},
    {"colorConversion", native_i420 ? color_conversion(conversion) : Json()},
    {"error", error.empty() ? Json(nullptr) : Json(error)}};
}
std::string VideoManager::open(const Json& options, int window, bool* created) {
  if (created) *created = false;
  auto& s = *impl_;
  if (!options.is_object() || window <= 0) throw Error("INVALID_ARGUMENT", "Expected video stream options and an App window");
  for (const auto& item : options.items()) if (item.key() != "source" && item.key() != "width" && item.key() != "height" && item.key() != "updateRate" && item.key() != "resolution" && item.key() != "preserveAspect" && item.key() != "format")
    throw Error("INVALID_ARGUMENT", "Unknown video option: " + item.key());
  if (options.value("source", Json("project")) != "project") throw Error("INVALID_ARGUMENT", "Only the current project video source is supported");
  int w = option(options, "width", 1280, 8192), h = option(options, "height", 720, 8192);
  const auto mode = options.value("resolution", Json("custom"));
  const auto format = options.value("format", Json("rgba8"));
  if (format != "rgba8" && format != "i420") throw Error("INVALID_ARGUMENT", "Unknown video pixel format");
  if (format == "i420" && mode != "source") throw Error("INVALID_ARGUMENT", "I420 requires source resolution");
  if (mode != "custom" && mode != "fit" && mode != "source") throw Error("INVALID_ARGUMENT", "Unknown video resolution mode");
  const auto aspect = options.value("preserveAspect", Json(true));
  if (!aspect.is_boolean()) throw Error("INVALID_ARGUMENT", "preserveAspect must be boolean");
  if (mode == "source" && (options.contains("width") || options.contains("height")))
    throw Error("INVALID_ARGUMENT", "Source resolution does not accept output dimensions");
  if (mode == "fit" && (!options.contains("width") || !options.contains("height")))
    throw Error("INVALID_ARGUMENT", "Fit resolution requires physical viewport width and height");
  const auto requested_rate = options.value("updateRate", Json("auto"));
  double rate = 0;
  if (requested_rate != "auto") {
    if (!requested_rate.is_number()) throw Error("INVALID_ARGUMENT", "updateRate must be auto or a number from 1 to 75");
    rate = requested_rate.get<double>();
    if (!std::isfinite(rate) || rate < 1 || rate > 75)
      throw Error("INVALID_ARGUMENT", "updateRate is outside the supported video range");
  }
  if (s.stopped || !s.backend->supported()) throw Error("VIDEO_HOST_UNSUPPORTED", "The REAPER video backend is unavailable on this host");
  if (format == "i420" && !s.backend->supports_i420()) throw Error("VIDEO_FORMAT_UNSUPPORTED", "Native I420 video is unavailable on this host");
  if (s.handle && s.had_consumers && !s.streams.consumers(s.handle)) s.close();
  int source_w = 0, source_h = 0;
  int matrix = -1;
  auto conversion = VideoColorConversion::unknown;
  if (mode != "custom") {
    const auto source = source_info();
    if (!source["available"].get<bool>()) throw Error(source["error"].get<std::string>(), "A valid final video frame is required to resolve output dimensions");
    source_w = source["width"]; source_h = source["height"];
    if (format == "i420") {
      if (source["nativeFormat"] != "i420" || source["colorMatrix"] != "bt709" || source["colorRange"] != "limited" ||
          (source["colorConversion"] != "reaper-neon" && source["colorConversion"] != "reaper-sse") ||
          (source_w & 15) || (source_h & 1)) throw Error("VIDEO_FORMAT_UNSUPPORTED", "The final video frame has no verified native I420 conversion");
      matrix = 1;
      conversion = source["colorConversion"] == "reaper-neon" ? VideoColorConversion::reaper_neon : VideoColorConversion::reaper_sse;
    }
    if (mode == "source") { w = source_w; h = source_h; }
    else if (aspect.get<bool>()) {
      const double scale = std::min({1.0, double(w) / source_w, double(h) / source_h});
      w = std::max(1, int(std::floor(source_w * scale))); h = std::max(1, int(std::floor(source_h * scale)));
    } else { w = std::min(w, source_w); h = std::min(h, source_h); }
  }
  if (w > 8192 || h > 8192 || uint64_t(w) * h * 4 > video_max_frame_bytes)
    throw Error("VIDEO_RESOLUTION_LIMIT", "Video output exceeds the frame pixel budget", {{"width", w}, {"height", h}, {"maxFrameBytes", video_max_frame_bytes}});
  if (s.handle) {
    if (format != s.format || mode != s.resolution || aspect != s.preserve_aspect || w != s.width || h != s.height || rate != s.rate) throw Error("VIDEO_PROFILE_CONFLICT", "Active project video consumers share one format, resolution and update rate", {{"format", s.format}, {"width", s.width}, {"height", s.height}, {"updateRate", s.requested_rate()}});
    s.idle_at = seconds(); return s.name;
  }
  s.probing = false;
  s.width = w; s.height = h; s.rate = rate; s.resolution = mode.get<std::string>(); s.preserve_aspect = aspect.get<bool>();
  s.format = format.get<std::string>(); s.matrix = matrix; s.conversion = conversion; s.native_i420 = format == "i420";
  s.resolved_source_width = source_w; s.resolved_source_height = source_h;
  s.project = s.host.current_project ? s.host.current_project() : nullptr;
  s.generation = s.host.project_generation ? s.host.project_generation() : 0;
  s.target_rate = s.resolve_rate(s.project_frame_rate());
  try {
    s.pixels.resize(format == "i420" ? size_t(w) * h * 3 / 2 : size_t(w) * h * 4);
    s.name = "reaweb.video.project." + std::to_string(++s.serial);
    ReaWeb_StreamDesc desc{};
    desc.size = sizeof(desc); desc.abi_version = REAWEB_STREAM_ABI; desc.format = format == "i420" ? REAWEB_PIXEL_I420 : REAWEB_PIXEL_RGBA8;
    desc.max_bytes = uint32_t(s.pixels.size()); desc.capacity = 3; desc.width = w; desc.height = h; desc.stride = format == "i420" ? w : w * 4;
    desc.update_rate = s.target_rate; desc.source = "project";
    const int result = s.streams.create(REAWEB_FRAME, s.name.c_str(), &desc, &s.handle, true,
      format == "i420" ? Json{{"colorMatrix", "bt709"}, {"colorRange", "limited"}, {"colorConversion", color_conversion(s.conversion)}} : Json());
    if (result) throw Error(StreamHub::code(result), "Cannot create project video stream");
    s.next_capture = s.first_capture = s.last_capture = 0;
    s.position_clock = {};
    s.produced = s.dropped = s.failed = s.captured = s.sequence = s.cleared = 0;
    s.source_rate = s.capture_ms = 0;
    s.valid = false; s.discontinuity = true; s.idle_at = seconds(); s.had_consumers = false;
    s.clear(0, "VIDEO_FRAME_PENDING");
    if (s.scheduler) s.scheduler->start(s.wake);
    if (created) *created = true;
    return s.name;
  } catch (...) { s.close(); throw; }
}
void VideoManager::discard_unattached(const std::string& name) {
  auto& s = *impl_;
  if (s.handle && s.name == name && !s.streams.consumers(s.handle)) s.close();
}
void VideoManager::tick() {
  auto& s = *impl_;
  if (!s.handle) {
    if (s.probing && seconds() - s.probe_started >= 5) {
      s.probing = false; s.backend->stop(); if (s.scheduler) s.scheduler->stop();
    }
    return;
  }
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
  if (s.position_clock.update(position, now, playing, play_rate, changes)) {
    s.discontinuity = true; s.clear(position, "VIDEO_FRAME_PENDING");
  }
  const double source_rate = s.project_frame_rate();
  const double target_rate = s.resolve_rate(source_rate);
  if (target_rate != s.target_rate) { s.target_rate = target_rate; s.next_capture = now; s.discontinuity = true; }
  // Auto follows distinct host frames instead of a second, unsynchronised clock.
  const bool follow_frames = !s.rate && (playing & 1) && source_rate > 0 && source_rate <= 75 &&
    std::isfinite(play_rate) && play_rate > 0 && play_rate <= 1;
  if (!follow_frames && now + 0.5 / s.target_rate < s.next_capture) return;
  bool copied = false, resized = false, format_changed = false;
  try {
    const auto capture = [&](const VideoImage& image) {
      if (s.format == "i420" && (image.format != VideoPixelFormat::i420 || image.color_matrix != s.matrix || image.color_conversion != s.conversion ||
          image.width != s.resolved_source_width || image.height != s.resolved_source_height)) {
        format_changed = true; return;
      }
      if (s.resolution != "custom" && (image.width != s.resolved_source_width || image.height != s.resolved_source_height)) {
        resized = true; return;
      }
      if (std::isfinite(position)) copied = s.capture(image, position, source_rate);
    };
    const auto result = s.format == "i420" ? s.backend->read_i420(s.discontinuity, capture) : s.backend->read(s.discontinuity, capture);
    if (format_changed || result == VideoRead::format_unsupported) { s.error = "VIDEO_FORMAT_CHANGED"; s.close(); return; }
    if (resized) { s.error = "VIDEO_RESOLUTION_CHANGED"; s.close(); return; }
    s.discontinuity = false;
    if (result != VideoRead::pending) s.pending_since = 0;
    if (result == VideoRead::unsupported) { s.error = "VIDEO_HOST_UNSUPPORTED"; s.close(); return; }
    if (result == VideoRead::unchanged || result == VideoRead::pending) {
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
  if (copied) s.capture_ms = (seconds() - now) * 1000;
}
void VideoManager::close_window(int window) { impl_->streams.detach_stream_window(impl_->handle, window); }
Json VideoManager::state() const {
  const auto& s = *impl_;
  const auto toggle = function<int(*)(int)>(s.host, "GetToggleCommandState");
  const bool visible = toggle && toggle(50125) > 0;
  return {{"source", "project"}, {"backend", "reaper-internal"},
    {"status", !s.handle ? "closed" : !s.had_consumers ? "idle" : s.valid ? "ready" : "waiting"}, {"epoch", s.epoch},
    {"width", s.width}, {"height", s.height}, {"resolution", s.resolution}, {"preserveAspect", s.preserve_aspect}, {"sourceWidth", s.source_width}, {"sourceHeight", s.source_height},
    {"format", s.format}, {"colorMatrix", s.format == "i420" ? color_matrix(s.matrix) : Json()},
    {"colorRange", s.format == "i420" ? Json("limited") : Json()},
    {"colorConversion", s.format == "i420" ? color_conversion(s.conversion) : Json()},
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
