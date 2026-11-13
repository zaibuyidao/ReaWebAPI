#pragma once
#include "core/core.hpp"

namespace reaweb {
enum class VideoPixelFormat { bgra8, i420 };
enum class VideoColorConversion { unknown, reaper_neon, reaper_sse, reaper_directdraw };
struct VideoImage {
  const unsigned char* pixels = nullptr;
  int width = 0, height = 0, stride = 0;
  VideoPixelFormat format = VideoPixelFormat::bgra8;
  bool native_i420 = false;
  int color_matrix = -1;
  VideoColorConversion color_conversion = VideoColorConversion::unknown;
  bool directdraw_hd = false;
};
enum class VideoRead { ready, unchanged, pending, empty, rendering, unsupported, format_unsupported };

// All methods and the borrowed image callback run on REAPER's main thread.
class VideoBackend {
public:
  using Capture = std::function<void(const VideoImage&)>;
  virtual ~VideoBackend() = default;
  virtual bool supported() = 0;
  virtual bool supports_i420() const { return false; }
  virtual VideoRead read(bool discontinuity, const Capture&) = 0;
  virtual VideoRead read_i420(bool, const Capture&) { return VideoRead::format_unsupported; }
  virtual VideoRead read_native_i420(bool changed, const Capture& capture) { return read_i420(changed, capture); }
  virtual VideoRead read_display_i420(bool, const Capture&) { return VideoRead::format_unsupported; }
  virtual void stop() = 0;
};
std::unique_ptr<VideoBackend> make_video_backend(const Host&);
}
