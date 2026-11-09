#pragma once
#include "core/core.hpp"

namespace reaweb {
struct VideoImage {
  const unsigned char* pixels = nullptr;
  int width = 0, height = 0, stride = 0;
};
enum class VideoRead { ready, unchanged, pending, empty, rendering, unsupported };

// All methods and the borrowed image callback run on REAPER's main thread.
class VideoBackend {
public:
  using Capture = std::function<void(const VideoImage&)>;
  virtual ~VideoBackend() = default;
  virtual bool supported() = 0;
  virtual VideoRead read(bool discontinuity, const Capture&) = 0;
  virtual void stop() = 0;
};
std::unique_ptr<VideoBackend> make_video_backend(const Host&);
}
