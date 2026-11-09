#pragma once
#include "video/internal.hpp"
#include <algorithm>

namespace reaweb {
// Image bounds and destination size are validated by VideoManager before conversion.
inline void copy_video_pixels(const VideoImage& image, int width, int height, std::vector<unsigned char>& pixels) {
  const int w = image.width, h = image.height;
  if (w == width && h == height) {
    for (int y = 0; y < h; ++y) {
      const auto* input = image.pixels + size_t(y) * image.stride;
      auto* output = pixels.data() + size_t(y) * width * 4;
      for (int x = 0; x < w; ++x, input += 4, output += 4) {
        output[0] = input[2]; output[1] = input[1]; output[2] = input[0]; output[3] = 255;
      }
    }
    return;
  }
  const double scale = std::min(double(width) / w, double(height) / h);
  const int dw = std::max(1, int(w * scale)), dh = std::max(1, int(h * scale));
  const int left = (width - dw) / 2, top = (height - dh) / 2;
  if (dw != width || dh != height) {
    std::fill(pixels.begin(), pixels.end(), 0);
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
  }
  const int step = w / dw, remainder = w % dw;
  for (int y = 0; y < dh; ++y) {
    const auto* input = image.pixels + size_t(std::min(h - 1, int(int64_t(y) * h / dh))) * image.stride;
    auto* output = pixels.data() + (size_t(y + top) * width + left) * 4;
    int source_x = 0, fraction = 0;
    for (int x = 0; x < dw; ++x, output += 4) {
      const auto* p = input + source_x * 4;
      // Integer phase accumulation preserves floor(x * w / dw) without per-pixel division.
      output[0] = p[2]; output[1] = p[1]; output[2] = p[0]; output[3] = 255;
      source_x += step; fraction += remainder;
      if (fraction >= dw) { ++source_x; fraction -= dw; }
    }
  }
}
}
