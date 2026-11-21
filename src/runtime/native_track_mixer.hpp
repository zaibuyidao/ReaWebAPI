#pragma once
#include "core/core.hpp"

#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
#define REAWEB_TRACK_WINDOWS_X64 1
#else
#define REAWEB_TRACK_WINDOWS_X64 0
#endif

#if REAWEB_TRACK_WINDOWS_X64 || (defined(__APPLE__) && defined(__aarch64__)) || (defined(__linux__) && defined(__x86_64__))
#define REAWEB_TRACK_RENDER_CACHE 1
#else
#define REAWEB_TRACK_RENDER_CACHE 0
#endif

namespace reaweb {
// The mixer supplies these samples on its own processing thread.
struct NativeTrackBlock {
  const void* track;
  const double* pcm;
  unsigned channels, frames;
  double rate, position;
  const void* gain;
  bool silence;
  void (*render_output)(const NativeTrackBlock&, double*, unsigned) noexcept;
};

class NativeTrackMixer {
public:
  using Observer = void (*)(void*, const NativeTrackBlock&) noexcept;
  ~NativeTrackMixer();
  NativeTrackMixer() = default;
  NativeTrackMixer(const NativeTrackMixer&) = delete;
  NativeTrackMixer& operator=(const NativeTrackMixer&) = delete;
  bool initialize(const Host& host, void* context, Observer observe);
  bool available() const noexcept { return state_ != nullptr; }
#if REAWEB_TRACK_RENDER_CACHE
  void hardware(bool post, void* project, void* track, int frames, double rate, uint64_t cycle) noexcept;
#endif
  void active(bool enabled) noexcept;
  void stop() noexcept;
private:
  void* state_ = nullptr;
};
}
