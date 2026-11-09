#pragma once
#include <functional>
#include <memory>

namespace reaweb {
class VideoScheduler {
public:
  virtual ~VideoScheduler() = default;
  virtual void start(std::function<void()> callback) = 0;
  virtual void stop() = 0;
};
std::unique_ptr<VideoScheduler> make_video_scheduler();
}
