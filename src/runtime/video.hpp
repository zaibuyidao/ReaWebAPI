#pragma once
#include "runtime/native_stream.hpp"
#include "video/internal.hpp"
#include "video/scheduler.hpp"

namespace reaweb {
class VideoManager {
public:
  VideoManager(const Host&, StreamHub&, std::unique_ptr<VideoBackend> backend = {},
    std::unique_ptr<VideoScheduler> scheduler = {}, std::function<void()> wake = {});
  ~VideoManager();
  Json capabilities() const;
  Json source_info();
  Json state() const;
  Json diagnostics() const;
  std::string open(const Json&, int window, bool* created = nullptr);
  void discard_unattached(const std::string& name);
  void tick();
  void close_window(int window);
  void shutdown();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
