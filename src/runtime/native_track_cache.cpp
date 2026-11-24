#include "runtime/native_track_mixer.hpp"

#if REAWEB_TRACK_RENDER_CACHE
#include "runtime/native_track_cache.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#if REAWEB_TRACK_WINDOWS_X64
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace reaweb {
namespace {
using Mix = NativeTrackCacheLayout::Mix;

struct CacheBlock {
  double position = 0, rate = 0;
  int64_t loop = 0;
  int frames = 0;
  std::array<double, 4> gains{};
};
struct Pending {
  const void* track = nullptr;
  const void* project = nullptr;
  uint64_t cycle = 0;
  double next_position = 0;
  bool capturing = false;
  CacheBlock current;
  std::array<CacheBlock, 8> deferred{};
  size_t count = 0;
};
struct State {
  NativeTrackMixer::Observer observer;
  void* context;
  NativeTrackCacheLayout layout;
  double (*position)();
  int64_t (*loop)(void*, int64_t*);
  int (*playing)();
  double (*track_info)(void*, const char*);
  std::array<Pending, 8> pending{};
};
struct Gains {
  Mix mix;
  const double* values;
};

template<class T> T field(const void* base, size_t offset) noexcept {
  T result;
  std::memcpy(&result, static_cast<const char*>(base) + offset, sizeof(T));
  return result;
}
void render_output(const NativeTrackBlock& block, double* samples, unsigned channels) noexcept {
  const auto& gains = *static_cast<const Gains*>(block.gain);
  const NativeTrackMixOutput out{1, samples, 0, int(channels), int(channels), 0};
  std::fill_n(samples, size_t(block.frames) * channels, 0.);
  gains.mix(block.pcm, &out, block.channels, block.frames, 0, gains.values, block.silence);
}
class TryLock {
public:
  TryLock(const void* base, size_t offset) {
    auto* lock = reinterpret_cast<Mutex*>(static_cast<char*>(const_cast<void*>(base)) + offset);
#if REAWEB_TRACK_WINDOWS_X64
    if (TryEnterCriticalSection(lock)) lock_ = lock;
#else
    if (!pthread_mutex_trylock(lock)) lock_ = lock;
#endif
  }
  ~TryLock() {
    if (!lock_) return;
#if REAWEB_TRACK_WINDOWS_X64
    LeaveCriticalSection(lock_);
#else
    pthread_mutex_unlock(lock_);
#endif
  }
  TryLock(const TryLock&) = delete;
  TryLock& operator=(const TryLock&) = delete;
  explicit operator bool() const { return lock_ != nullptr; }
private:
#if REAWEB_TRACK_WINDOWS_X64
  using Mutex = CRITICAL_SECTION;
#else
  using Mutex = pthread_mutex_t;
#endif
  Mutex* lock_ = nullptr;
};
bool emit_block(State& state, const void* track, const CacheBlock& block) noexcept {
  const Gains gain{state.layout.mix, block.gains.data()};
  // RenderSamples stores its post-FX output in cache list 1. Its own
  // GetCachedRendered may unlink and recycle entries, so only walk the list
  // while holding the same cache mutex. Consume precisely this transport block.
  auto* node = field<const void*>(track, state.layout.cache_head);
  for (int i = 0; node && i < 256; ++i, node = field<const void*>(node, 0)) {
    const double start = field<double>(node, 0x18);
    if (field<int64_t>(node, 8) != block.loop || !std::isfinite(start) || std::abs(start - block.position) > .5 / block.rate ||
        field<int>(node, 0x14) != block.frames || field<int>(node, 0x24) < block.frames) continue;
    const int channels = field<int>(node, 0x20);
    const bool silence = field<uint8_t>(node, 0x11) != 0;
    auto* pcm = field<const double*>(node, 0x38);
    if (channels < 1 || channels > 128 || (!silence && (!pcm || field<int>(node, 0x44) < int(sizeof(double)) * block.frames * channels))) return false;
    state.observer(state.context, {track, pcm, unsigned(channels), unsigned(block.frames), block.rate, start, &gain, silence, render_output});
    return true;
  }
  return false;
}
bool emit_uncached(State& state, void* track, const CacheBlock& block, double end) noexcept {
  const auto& layout = state.layout.uncached;
  if (!layout.buffer) return false;
  // The render worker claims this buffer under a separate mutex before writing it.
  const TryLock lock(track, layout.mutex);
  if (!lock || field<uint8_t>(track, layout.busy)) return false;
  const double position = field<double>(track, layout.position);
  if (!std::isfinite(position) || std::abs(position - end) > .5 / block.rate ||
      field<int64_t>(track, layout.position + 8) != block.loop ||
      field<int>(track, layout.position + 16) != 0 ||
      std::abs(end - block.position - block.frames / block.rate) > .5 / block.rate) return false;
  const auto channel_count = state.track_info(track, "I_NCHAN");
  if (!(channel_count >= 1 && channel_count <= 128)) return false;
  const auto channels = static_cast<unsigned>(channel_count);
  const auto* pcm = field<const double*>(track, layout.buffer);
  const int bytes = block.frames * channels * 2 * sizeof(double);
  if (!pcm || field<int>(track, layout.buffer + 12) != bytes ||
      field<int>(track, layout.buffer + 8) < bytes) return false;
  const Gains gain{state.layout.mix, block.gains.data()};
  state.observer(state.context, {track, pcm, channels, unsigned(block.frames), block.rate, block.position,
                                 &gain, false, render_output});
  return true;
}
void retry_before_render(State& state, Pending& pending) noexcept {
  size_t emitted = 0;
  while (emitted < pending.count && emit_block(state, pending.track, pending.deferred[emitted]))
    ++emitted;
  if (emitted) {
    std::move(pending.deferred.begin() + emitted, pending.deferred.begin() + pending.count, pending.deferred.begin());
    pending.count -= emitted;
  }
}

}

bool NativeTrackMixer::initialize(const Host& host, void* context, Observer observer) {
  if (state_) return true;
  const auto layout = native_track_cache_layout();
  if (!host.native_function || !observer || !layout.mix) return false;
  auto state = std::make_unique<State>();
  state->observer = observer;
  state->context = context;
  state->layout = layout;
  state->position = reinterpret_cast<decltype(state->position)>(host.native_function("GetPlayPosition2"));
  state->loop = reinterpret_cast<decltype(state->loop)>(host.native_function("GetPlayLoopCnt"));
  state->playing = reinterpret_cast<decltype(state->playing)>(host.native_function("GetPlayState"));
  state->track_info = reinterpret_cast<decltype(state->track_info)>(host.native_function("GetMediaTrackInfo_Value"));
  if (!state->position || !state->loop || !state->playing || !state->track_info) return false;
  state_ = state.release();
  return true;
}
void NativeTrackMixer::active(bool) noexcept {}
void NativeTrackMixer::stop() noexcept {
  delete static_cast<State*>(state_);
  state_ = nullptr;
}

void NativeTrackMixer::hardware(bool post, void* project, void* track, int frames, double rate, uint64_t cycle) noexcept {
  auto* state = static_cast<State*>(state_);
  if (!state || !track || frames < 1 || frames > 8192 || !std::isfinite(rate) || rate < 8000 || rate > 768000) return;
  const auto same_track = [&](const Pending& pending) { return pending.track == track; };
  auto pending = std::find_if(state->pending.begin(), state->pending.end(), same_track);
  if (!post) {
    if (pending == state->pending.end()) {
      pending = std::min_element(state->pending.begin(), state->pending.end(),
          [](const Pending& a, const Pending& b) { return a.cycle < b.cycle; });
      pending->track = track;
      pending->count = 0;
    }
    pending->capturing = (state->playing() & 1) && state->track_info(track, "B_MAINSEND");
    const auto last_cycle = pending->cycle;
    pending->cycle = cycle;
    if (!pending->capturing) { pending->count = 0; return; }

    auto& block = pending->current;
    block.position = state->position();
    block.loop = state->loop(project, nullptr);
    block.rate = rate;
    block.frames = frames;
    block.gains[2] = field<double>(track, state->layout.previous_gains);
    block.gains[3] = field<double>(track, state->layout.previous_gains + sizeof(double));
    if (pending->count) {
      const auto& last = pending->deferred[pending->count - 1];
      if (pending->project != project || last_cycle + 1 != cycle || last.loop != block.loop ||
          last.rate != rate || !std::isfinite(block.position) ||
          std::abs(pending->next_position - block.position) > .5 / rate) {
        pending->count = 0;
      } else {
        TryLock lock(track, state->layout.cache_mutex);
        if (lock) retry_before_render(*state, *pending);
      }
    }
    pending->project = project;
    return;
  }
  if (pending == state->pending.end() || pending->cycle != cycle || !pending->capturing) return;
  pending->capturing = false;
  auto& block = pending->current;
  pending->next_position = state->position();
  // Stationary prebuffering can produce a predicted node before playback starts.
  if (!std::isfinite(block.position) || !std::isfinite(pending->next_position) ||
      pending->next_position <= block.position) return;
  block.gains[0] = field<double>(track, state->layout.previous_gains);
  block.gains[1] = field<double>(track, state->layout.previous_gains + sizeof(double));
  bool checked_cache = false;
  {
    TryLock lock(track, state->layout.cache_mutex);
    if (lock) {
      // Publish retained older blocks first. Nodes already recycled by the host
      // cannot be reconstructed from a later block.
      for (size_t i = 0; i < pending->count; ++i)
        emit_block(*state, track, pending->deferred[i]);
      pending->count = 0;
      if (emit_block(*state, track, block)) return;
      checked_cache = true;
    }
  }
  if (checked_cache && emit_uncached(*state, track, block, pending->next_position)) return;
  // Keep the completed block's identity and gains if the render worker owns
  // the lock or the PCM is not visible yet. Never retain host PCM pointers.
  if (pending->count == pending->deferred.size()) {
    std::move(pending->deferred.begin() + 1, pending->deferred.end(), pending->deferred.begin());
    --pending->count;
  }
  pending->deferred[pending->count++] = block;
}
}
#endif
