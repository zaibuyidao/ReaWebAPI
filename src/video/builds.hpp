#pragma once
#include <cstddef>
#include <string_view>

namespace reaweb {
struct VideoBuild {
  const char* checksum;
  size_t display, visible, window, frame, processed, monitor_hash;
  size_t query, source_hash, cached, destroy_sources, frame_index, is_visible, transform;
  size_t yuv_gamut = 0, converter = 0, codec_version = 0, sources_at = 0;
  size_t renderer = 0, directdraw_vtable = 0, video_colorspace = 0;
};
// Verified module layouts. Version labels are descriptive, never used for selection.
#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
// REAPER 7.84 / 7.85
inline constexpr VideoBuild video_builds[]{{"b50bc6486f996acc2731acc6889217fdfb8f55dd8e6524746e7d7582ff705f59",
  0x1a8848, 0x26c14c, 0x26bd70, 0x26bdb0, 0x26bda8, 0x26bda0,
  0x1a9738, 0x1a5fc0, 0x1af738, 0x1ad4dc, 0x26bd98, 0x211810, 0x1a6ffc, 0x1a70e8, 0x266af4, 0x266af0, 0x1a50a0, 0x26bdb8, 0x280148, 0x26be30},
  // REAPER 7.86
  {"0d45a837353b064a670e7bcd29a7835d462ae99cea030a291406c0a8f991bd23",
  0x1a8acc, 0x26c14c, 0x26bd70, 0x26bdb0, 0x26bda8, 0x26bda0,
  0x1a99bc, 0x1a6244, 0x1af9bc, 0x1ad760, 0x26bd98, 0x211810, 0x1a7280, 0x1a736c, 0x266af4, 0x266af0, 0x1a5324, 0x26bdb8, 0x280148, 0x26be30}};
#elif defined(__APPLE__) && defined(__aarch64__)
// REAPER 7.84 / 7.85
inline constexpr VideoBuild video_builds[]{{"318fef1c8d990840587b26cc58e2b7e9659844156b8578ba540e493c09f32ba0",
  0x13868, 0x110860, 0x110858, 0x110ac8, 0x110ad0, 0x110ad8,
  0x13100, 0x126ac, 0x12a94, 0x12054, 0x110ae0, 0x10fa28, 0x14fdc, 0x11954, 0, 0, 0x12bc4},
  // REAPER 7.86
  {"7b76d4b81e9039545caf027222564237e6fd5b2a609661f28d4fed581376b9c4",
  0x13834, 0x110860, 0x110858, 0x110ac8, 0x110ad0, 0x110ad8,
  0x130cc, 0x12678, 0x12a60, 0x12020, 0x110ae0, 0x10fa28, 0x14fa8, 0x11920, 0, 0, 0x12b90}};
#elif defined(__linux__) && defined(__x86_64__)
// REAPER 7.82, 7.84, 7.85, 7.86
inline constexpr VideoBuild video_builds[]{{"f34a2e7634400dedfc70757e5daa7861988529e16ddcb2a3a85c5b72841e9535",
  0x1382f0, 0x3f8148, 0x3f8150, 0x3f7e78, 0x3f7e70, 0x3f7e68,
  0x132b30, 0x12b270, 0x12b660, 0x12ad30, 0x3f7e60, 0x3f21c0, 0x12e590, 0x12a460, 0x3f274c, 0x3f2748, 0x12b7b0},
  {"ef1bb206879f4a73f80550f551b98bb00b9493dd98648421c70c3d6eda3790ee",
  0x138330, 0x3f8148, 0x3f8150, 0x3f7e78, 0x3f7e70, 0x3f7e68,
  0x132b70, 0x12b2b0, 0x12b6a0, 0x12ad70, 0x3f7e60, 0x3f21c0, 0x12e5d0, 0x12a4a0, 0x3f274c, 0x3f2748, 0x12b7f0},
  {"29efc78f2099a22a5a5c3cd31c0136f31af0d14ad619901fd7d0e914a2a7f3fd",
  0x138320, 0x3f8148, 0x3f8150, 0x3f7e78, 0x3f7e70, 0x3f7e68,
  0x133d60, 0x12b2a0, 0x12b690, 0x12ad60, 0x3f7e60, 0x3f21c0, 0x12e5c0, 0x12a490, 0x3f274c, 0x3f2748, 0x12b7e0},
  {"5f66735602a1d21ea17bbf6d8880f098bfa6d6de05eb39ac8ade195ca7e2d47c",
  0x138570, 0x3f8148, 0x3f8150, 0x3f7e78, 0x3f7e70, 0x3f7e68,
  0x133fb0, 0x12b4f0, 0x12b8e0, 0x12afb0, 0x3f7e60, 0x3f21c0, 0x12e810, 0x12a6e0, 0x3f274c, 0x3f2748, 0x12ba30}};
#else
inline constexpr VideoBuild video_builds[]{{}};
#endif

inline const VideoBuild* find_video_build(std::string_view fingerprint) noexcept {
  for (const auto& candidate : video_builds)
    if (candidate.checksum && fingerprint == candidate.checksum) return &candidate;
  return nullptr;
}
}
