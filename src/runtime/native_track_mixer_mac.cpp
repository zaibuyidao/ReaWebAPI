#include "runtime/native_track_mixer.hpp"

#if defined(__APPLE__) && defined(__aarch64__)
#include "runtime/native_track_cache.hpp"
#include <cstdint>
#include <cstring>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <pthread.h>

namespace reaweb {
namespace {
struct Region {
  uintptr_t start;
  size_t size;
  bool contains(uintptr_t address, size_t length) const noexcept {
    return start && address >= start && length <= size && address - start <= size - length;
  }
};

Region section(const mach_header_64* image, const char* segment, const char* name) {
  unsigned long size = 0;
  const auto* data = getsectiondata(image, segment, name, &size);
  return {reinterpret_cast<uintptr_t>(data), size};
}

template<class T> T load(uintptr_t address) noexcept {
  T value;
  std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
  return value;
}

// FNV-1a over position-independent instruction windows from REAPER 7.86 arm64.
// Offsets are within the candidate function and never relative to the image.
struct Instructions { size_t offset, length; uint64_t hash; };
constexpr Instructions mix_windows[]{
  {0, 0x6c, 0x4a49423491b841db},       // Output channels, stride, buffer and silence arguments.
  {0x1a0, 0x1e8, 0x79e2016d3aebf5a4}, // Gain ramp.
  {0x38c, 0x654, 0x298d8b380965b8d9}, // Scalar and vector sample loops.
};
constexpr Instructions render_windows[]{
  {0, 0x28, 0xcdb235cdf04f4e99},
  {0x200, 0xc, 0xcf87c7909fceb5b3},   // track + 0x35a8 passed to pthread_mutex_lock.
  {0x210, 0x24, 0x8acd1958886f310b}, // GetCachedRendered(track, ..., list 1).
  {0x834, 0x38, 0x1b86d6e1098a0227}, // Load and update the stereo gain pair at track + 0xe8.
  {0x884, 0x10, 0x459c44c548251798}, // Node buffer extent (+0x44) and PCM pointer (+0x38).
  {0x8c8, 0x48, 0x9cb5105eca784537}, // Channel/frame counts, time, list head (+0x2590) and next link.
  {0xc50, 0x18, 0x51485c5a69f2307f}, // Channels (+0x20), silence (+0x11), gains and mixer arguments.
};
constexpr Instructions cache_windows[]{
  {0, 0x4c, 0x4f70b739eae71836},
  {0x98, 0x1c, 0xfc65cf1b56e90b0c}, // Indexed cache lists.
  {0x118, 0xa4, 0x308eb9705b950628}, // Match loop (+8), start (+0x18), frames (+0x14) and walk next.
};

template<size_t N>
uintptr_t unique_function(Region text, uint32_t first, const Instructions (&windows)[N]) noexcept {
  const size_t length = windows[N - 1].offset + windows[N - 1].length;
  uintptr_t found = 0;
  for (size_t i = 0; i <= text.size && length <= text.size - i; i += sizeof(uint32_t)) {
    const auto entry = text.start + i;
    if (load<uint32_t>(entry) != first) continue;
    bool matches = true;
    for (const auto& window : windows) {
      uint64_t hash = 0xcbf29ce484222325ULL;
      const auto* bytes = reinterpret_cast<const uint8_t*>(entry + window.offset);
      for (size_t j = 0; j < window.length; ++j) hash = (hash ^ bytes[j]) * 0x100000001b3ULL;
      if (hash != window.hash) { matches = false; break; }
    }
    if (matches) {
      if (found) return 0;
      found = entry;
    }
  }
  return found;
}

int64_t signed_immediate(uint32_t value, unsigned bits) noexcept {
  return int64_t(value) - ((value >> (bits - 1)) ? (int64_t{1} << bits) : 0);
}

uintptr_t called_function(uintptr_t pc) noexcept {
  const auto word = load<uint32_t>(pc);
  if ((word >> 26) != 0x25) return 0; // BL imm26.
  return pc + signed_immediate(word & 0x03ffffff, 26) * 4;
}

// Verify the arm64 import stub (ADRP / LDR / BR x16) and its bound pointer.
bool calls_import(uintptr_t pc, Region stubs, Region imports, uintptr_t target) noexcept {
  const auto stub = called_function(pc);
  if (!stubs.contains(stub, 12)) return false;
  const uint32_t page = load<uint32_t>(stub), ldr = load<uint32_t>(stub + 4);
  if ((page & 0x9f00001f) != 0x90000010 || (ldr & 0xffc003ff) != 0xf9400210 ||
      load<uint32_t>(stub + 8) != 0xd61f0200) return false;
  const uint32_t displacement = ((page >> 5) & 0x7ffff) * 4 + ((page >> 29) & 3);
  const auto slot = (stub & ~uintptr_t{0xfff}) + signed_immediate(displacement, 21) * 4096 +
                    ((ldr >> 10) & 0xfff) * 8;
  return imports.contains(slot, sizeof(uintptr_t)) && load<uintptr_t>(slot) == target;
}

NativeTrackCacheLayout resolve_layout(Region text, Region stubs, Region imports) noexcept {
  if (!text.start) return {};
  const auto mix = unique_function(text, 0xa9bc5ff8, mix_windows);
  const auto render = unique_function(text, 0xd10503ff, render_windows);
  const auto cache = unique_function(text, 0xd10343ff, cache_windows);
  if (!mix || !render || !cache || !text.contains(render, 0xc6c) ||
      called_function(render + 0xc68) != mix || called_function(render + 0x234) != cache ||
      !calls_import(render + 0x20c, stubs, imports, reinterpret_cast<uintptr_t>(&pthread_mutex_lock)) ||
      load<uint32_t>(render + 0x2f0) != 0xf94033e0 || // Reload the same lock from sp + 0x60.
      !calls_import(render + 0x2f4, stubs, imports, reinterpret_cast<uintptr_t>(&pthread_mutex_unlock)))
    return {};
  return {reinterpret_cast<NativeTrackCacheLayout::Mix>(mix), 0x35a8, 0x2590, 0xe8};
}
}

NativeTrackCacheLayout native_track_cache_layout() noexcept {
  const auto* image = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
  if (!image || image->magic != MH_MAGIC_64 || image->cputype != CPU_TYPE_ARM64) return {};
  return resolve_layout(section(image, "__TEXT", "__text"), section(image, "__TEXT", "__stubs"),
                        section(image, "__DATA", "__la_symbol_ptr"));
}
}
#endif
