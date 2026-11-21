#pragma once
#include <cstddef>
#include <filesystem>

namespace reaweb {
struct VideoBuild {
  size_t display, visible, window, frame, processed, monitor_hash;
  size_t query, source_hash, cached, destroy_sources, frame_index, is_visible, transform;
  size_t yuv_gamut = 0, converter = 0, codec_version = 0, sources_at = 0;
  size_t renderer = 0, directdraw_vtable = 0, video_colorspace = 0;
};

// All offsets are relative to the loaded video module. Publish a layout only
// after validating every entry required by the platform's frame reader.
#if (defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))) || \
    (defined(__APPLE__) && defined(__aarch64__)) || (defined(__linux__) && defined(__x86_64__))
bool resolve_video_layout(const unsigned char* base, const std::filesystem::path& path, VideoBuild& result);
#else
inline bool resolve_video_layout(const unsigned char*, const std::filesystem::path&, VideoBuild&) { return false; }
#endif
}
