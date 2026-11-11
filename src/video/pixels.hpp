#pragma once
#include "video/internal.hpp"
#include <algorithm>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace reaweb {
// Image bounds and destination size are validated by VideoManager before conversion.
inline void copy_video_pixels(const VideoImage& image, int width, int height, std::vector<unsigned char>& pixels, bool preserve_aspect = true) {
  const int w = image.width, h = image.height;
  if (w == width && h == height) {
    for (int y = 0; y < h; ++y) {
      const auto* input = image.pixels + size_t(y) * image.stride;
      auto* output = pixels.data() + size_t(y) * width * 4;
      int x = 0;
#if defined(__SSE2__) || defined(_M_X64)
      const auto low = _mm_set1_epi32(0xff), green = _mm_set1_epi32(0xff00);
      const auto alpha = _mm_set1_epi32(-16777216);
      for (; x + 4 <= w; x += 4, input += 16, output += 16) {
        const auto bgra = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
        const auto red = _mm_and_si128(_mm_srli_epi32(bgra, 16), low);
        const auto blue = _mm_slli_epi32(_mm_and_si128(bgra, low), 16);
        const auto rgba = _mm_or_si128(_mm_or_si128(red, blue), _mm_or_si128(_mm_and_si128(bgra, green), alpha));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output), rgba);
      }
#endif
      for (; x < w; ++x, input += 4, output += 4) {
        output[0] = input[2]; output[1] = input[1]; output[2] = input[0]; output[3] = 255;
      }
    }
    return;
  }
  const double scale = std::min(double(width) / w, double(height) / h);
  const int dw = preserve_aspect ? std::max(1, int(w * scale)) : width;
  const int dh = preserve_aspect ? std::max(1, int(h * scale)) : height;
  const int left = (width - dw) / 2, top = (height - dh) / 2;
  if (dw != width || dh != height) {
    std::fill(pixels.begin(), pixels.end(), 0);
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
  }
  struct Sample { int first, second, weight; };
  const auto sample = [](int at, int input, int output) {
    const double position = std::clamp((at + 0.5) * input / output - 0.5, 0.0, double(input - 1));
    const int first = int(position);
    return Sample{first, std::min(first + 1, input - 1), int((position - first) * 256 + 0.5)};
  };
  std::vector<Sample> columns;
  columns.reserve(dw);
  for (int x = 0; x < dw; ++x) columns.push_back(sample(x, w, dw));
  for (int y = 0; y < dh; ++y) {
    const auto row = sample(y, h, dh);
    const auto* a = image.pixels + size_t(row.first) * image.stride;
    const auto* b = image.pixels + size_t(row.second) * image.stride;
    auto* output = pixels.data() + (size_t(y + top) * width + left) * 4;
    for (int x = 0; x < dw; ++x, output += 4) {
      const auto col = columns[x];
      for (int channel = 0; channel < 3; ++channel) {
        const int c = 2 - channel;
        const int upper = a[col.first * 4 + c] * (256 - col.weight) + a[col.second * 4 + c] * col.weight;
        const int lower = b[col.first * 4 + c] * (256 - col.weight) + b[col.second * 4 + c] * col.weight;
        output[channel] = static_cast<unsigned char>((upper * (256 - row.weight) + lower * row.weight + 32768) >> 16);
      }
      output[3] = 255;
    }
  }
}
}
