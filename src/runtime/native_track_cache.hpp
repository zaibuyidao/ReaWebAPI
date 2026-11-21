#pragma once
#include <cstddef>

namespace reaweb {
struct NativeTrackMixOutput {
  double volume;
  double* samples;
  int mode, channels, stride, offset;
};
static_assert(sizeof(NativeTrackMixOutput) == 32);

// Only returned for a host whose track, cache node and gain mixer ABI match.
struct NativeTrackCacheLayout {
  using Mix = void (*)(const double*, const NativeTrackMixOutput*, int, int, int, const double*, bool);
  Mix mix = nullptr;
  std::size_t cache_mutex = 0, cache_head = 0, previous_gains = 0;
};
NativeTrackCacheLayout native_track_cache_layout() noexcept;
}
