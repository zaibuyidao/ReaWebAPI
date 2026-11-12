#pragma once
#include "video/internal.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace reaweb {
namespace detail {
inline void fill_opaque_black(unsigned char* dst, size_t pixels) {
#if defined(__SSE2__) || defined(_M_X64)
  const auto black = _mm_set1_epi32(-16777216);
  size_t i = 0;
  for (; i + 4 <= pixels; i += 4) _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i * 4), black);
  for (; i < pixels; ++i) { dst[i * 4] = dst[i * 4 + 1] = dst[i * 4 + 2] = 0; dst[i * 4 + 3] = 255; }
#else
  for (size_t i = 0; i < pixels; ++i) { dst[i * 4] = dst[i * 4 + 1] = dst[i * 4 + 2] = 0; dst[i * 4 + 3] = 255; }
#endif
}

inline void i420_row_to_bgra(const VideoImage& image, int y, unsigned char* out) {
  const int w = image.width, h = image.height, stride = image.stride, cstride = stride / 2;
  const auto* y_row = image.pixels + size_t(y) * stride;
  const auto* u_row = image.pixels + size_t(h) * stride + size_t(y / 2) * cstride;
  const auto* v_row = image.pixels + size_t(h) * stride + size_t(h / 2 + y / 2) * cstride;
  const bool sse = image.color_conversion == VideoColorConversion::reaper_sse;
  int x = 0;
#if defined(__SSE2__) || defined(_M_X64)
  if (sse) {
    const auto zero = _mm_setzero_si128();
    const auto c16 = _mm_set1_epi16(16), c128 = _mm_set1_epi16(128);
    const auto cy = _mm_set1_epi16(9539), crv = _mm_set1_epi16(14686);
    const auto cgu = _mm_set1_epi16(-1747), cgv = _mm_set1_epi16(-4366), cbu = _mm_set1_epi16(17305);
    const auto low = _mm_set1_epi16(0x00ff), alpha = _mm_set1_epi16(-256);
    for (; x + 8 <= w; x += 8, out += 32) {
      const auto y8 = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(y_row + x)), zero);
      const auto y_val = _mm_srai_epi16(_mm_mulhi_epi16(_mm_slli_epi16(_mm_sub_epi16(y8, c16), 3), cy), 0);
      int32_t u4_raw, v4_raw;
      std::memcpy(&u4_raw, u_row + (x >> 1), 4);
      std::memcpy(&v4_raw, v_row + (x >> 1), 4);
      const auto u4 = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(u4_raw), zero), c128);
      const auto v4 = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(v4_raw), zero), c128);
      const auto u8 = _mm_unpacklo_epi16(u4, u4);
      const auto v8 = _mm_unpacklo_epi16(v4, v4);
      const auto rv = _mm_mulhi_epi16(_mm_slli_epi16(v8, 3), crv);
      const auto guv = _mm_add_epi16(_mm_mulhi_epi16(_mm_slli_epi16(u8, 3), cgu), _mm_mulhi_epi16(_mm_slli_epi16(v8, 3), cgv));
      const auto bu = _mm_mulhi_epi16(_mm_slli_epi16(u8, 3), cbu);
      const auto r8 = _mm_packus_epi16(_mm_add_epi16(y_val, rv), zero);
      const auto g8 = _mm_packus_epi16(_mm_add_epi16(y_val, guv), zero);
      const auto b8 = _mm_packus_epi16(_mm_add_epi16(y_val, bu), zero);
      const auto bg = _mm_unpacklo_epi8(b8, g8);
      const auto ra = _mm_or_si128(_mm_and_si128(_mm_unpacklo_epi8(r8, zero), low), alpha);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out), _mm_unpacklo_epi16(bg, ra));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16), _mm_unpackhi_epi16(bg, ra));
    }
  }
#endif
  for (; x < w; x += 2, out += 8) {
    const int u = int(u_row[x >> 1]) - 128;
    const int v = int(v_row[x >> 1]) - 128;
    if (sse) {
      const int rv = (v * 14686) >> 13;
      const int guv = ((u * -1747) >> 13) + ((v * -4366) >> 13);
      const int bu = (u * 17305) >> 13;
      const int y0 = ((int(y_row[x]) - 16) * 9539) >> 13;
      const int y1 = ((int(y_row[x + 1]) - 16) * 9539) >> 13;
      out[0] = static_cast<unsigned char>(std::clamp(y0 + bu, 0, 255));
      out[1] = static_cast<unsigned char>(std::clamp(y0 + guv, 0, 255));
      out[2] = static_cast<unsigned char>(std::clamp(y0 + rv, 0, 255));
      out[3] = 255;
      out[4] = static_cast<unsigned char>(std::clamp(y1 + bu, 0, 255));
      out[5] = static_cast<unsigned char>(std::clamp(y1 + guv, 0, 255));
      out[6] = static_cast<unsigned char>(std::clamp(y1 + rv, 0, 255));
      out[7] = 255;
    } else {
      const int rv = (v * 14686) >> 12;
      const int guv = ((u * -1747) >> 12) + ((v * -4366) >> 12);
      const int bu = (u * 17305) >> 12;
      const int y0 = ((int(y_row[x]) - 16) * 9539) >> 12;
      const int y1 = ((int(y_row[x + 1]) - 16) * 9539) >> 12;
      out[0] = static_cast<unsigned char>(std::clamp((y0 + bu + 1) >> 1, 0, 255));
      out[1] = static_cast<unsigned char>(std::clamp((y0 + guv + 1) >> 1, 0, 255));
      out[2] = static_cast<unsigned char>(std::clamp((y0 + rv + 1) >> 1, 0, 255));
      out[3] = 255;
      out[4] = static_cast<unsigned char>(std::clamp((y1 + bu + 1) >> 1, 0, 255));
      out[5] = static_cast<unsigned char>(std::clamp((y1 + guv + 1) >> 1, 0, 255));
      out[6] = static_cast<unsigned char>(std::clamp((y1 + rv + 1) >> 1, 0, 255));
      out[7] = 255;
    }
  }
}

inline void i420_row_to_rgba(const VideoImage& image, int y, unsigned char* out) {
  const int w = image.width, h = image.height, stride = image.stride, cstride = stride / 2;
  const auto* y_row = image.pixels + size_t(y) * stride;
  const auto* u_row = image.pixels + size_t(h) * stride + size_t(y / 2) * cstride;
  const auto* v_row = image.pixels + size_t(h) * stride + size_t(h / 2 + y / 2) * cstride;
  const bool sse = image.color_conversion == VideoColorConversion::reaper_sse;
  int x = 0;
#if defined(__SSE2__) || defined(_M_X64)
  if (sse) {
    const auto zero = _mm_setzero_si128();
    const auto c16 = _mm_set1_epi16(16), c128 = _mm_set1_epi16(128);
    const auto cy = _mm_set1_epi16(9539), crv = _mm_set1_epi16(14686);
    const auto cgu = _mm_set1_epi16(-1747), cgv = _mm_set1_epi16(-4366), cbu = _mm_set1_epi16(17305);
    const auto low = _mm_set1_epi16(0x00ff), alpha = _mm_set1_epi16(-256);
    for (; x + 8 <= w; x += 8, out += 32) {
      const auto y8 = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(y_row + x)), zero);
      const auto y_val = _mm_srai_epi16(_mm_mulhi_epi16(_mm_slli_epi16(_mm_sub_epi16(y8, c16), 3), cy), 0);
      int32_t u4_raw, v4_raw;
      std::memcpy(&u4_raw, u_row + (x >> 1), 4);
      std::memcpy(&v4_raw, v_row + (x >> 1), 4);
      const auto u4 = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(u4_raw), zero), c128);
      const auto v4 = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(v4_raw), zero), c128);
      const auto u8 = _mm_unpacklo_epi16(u4, u4);
      const auto v8 = _mm_unpacklo_epi16(v4, v4);
      const auto rv = _mm_mulhi_epi16(_mm_slli_epi16(v8, 3), crv);
      const auto guv = _mm_add_epi16(_mm_mulhi_epi16(_mm_slli_epi16(u8, 3), cgu), _mm_mulhi_epi16(_mm_slli_epi16(v8, 3), cgv));
      const auto bu = _mm_mulhi_epi16(_mm_slli_epi16(u8, 3), cbu);
      const auto r8 = _mm_packus_epi16(_mm_add_epi16(y_val, rv), zero);
      const auto g8 = _mm_packus_epi16(_mm_add_epi16(y_val, guv), zero);
      const auto b8 = _mm_packus_epi16(_mm_add_epi16(y_val, bu), zero);
      const auto rg = _mm_unpacklo_epi8(r8, g8);
      const auto ba = _mm_or_si128(_mm_and_si128(_mm_unpacklo_epi8(b8, zero), low), alpha);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out), _mm_unpacklo_epi16(rg, ba));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16), _mm_unpackhi_epi16(rg, ba));
    }
  }
#endif
  for (; x < w; x += 2, out += 8) {
    const int u = int(u_row[x >> 1]) - 128;
    const int v = int(v_row[x >> 1]) - 128;
    if (sse) {
      const int rv = (v * 14686) >> 13;
      const int guv = ((u * -1747) >> 13) + ((v * -4366) >> 13);
      const int bu = (u * 17305) >> 13;
      const int y0 = ((int(y_row[x]) - 16) * 9539) >> 13;
      const int y1 = ((int(y_row[x + 1]) - 16) * 9539) >> 13;
      out[0] = static_cast<unsigned char>(std::clamp(y0 + rv, 0, 255));
      out[1] = static_cast<unsigned char>(std::clamp(y0 + guv, 0, 255));
      out[2] = static_cast<unsigned char>(std::clamp(y0 + bu, 0, 255));
      out[3] = 255;
      out[4] = static_cast<unsigned char>(std::clamp(y1 + rv, 0, 255));
      out[5] = static_cast<unsigned char>(std::clamp(y1 + guv, 0, 255));
      out[6] = static_cast<unsigned char>(std::clamp(y1 + bu, 0, 255));
      out[7] = 255;
    } else {
      const int rv = (v * 14686) >> 12;
      const int guv = ((u * -1747) >> 12) + ((v * -4366) >> 12);
      const int bu = (u * 17305) >> 12;
      const int y0 = ((int(y_row[x]) - 16) * 9539) >> 12;
      const int y1 = ((int(y_row[x + 1]) - 16) * 9539) >> 12;
      out[0] = static_cast<unsigned char>(std::clamp((y0 + rv + 1) >> 1, 0, 255));
      out[1] = static_cast<unsigned char>(std::clamp((y0 + guv + 1) >> 1, 0, 255));
      out[2] = static_cast<unsigned char>(std::clamp((y0 + bu + 1) >> 1, 0, 255));
      out[3] = 255;
      out[4] = static_cast<unsigned char>(std::clamp((y1 + rv + 1) >> 1, 0, 255));
      out[5] = static_cast<unsigned char>(std::clamp((y1 + guv + 1) >> 1, 0, 255));
      out[6] = static_cast<unsigned char>(std::clamp((y1 + bu + 1) >> 1, 0, 255));
      out[7] = 255;
    }
  }
}
}

// Image bounds and destination size are validated by VideoManager before conversion.
inline void copy_video_pixels(const VideoImage& image, int width, int height, std::vector<unsigned char>& pixels, bool preserve_aspect = true) {
  const int w = image.width, h = image.height;
  const bool i420 = image.format == VideoPixelFormat::i420;
  if (w == width && h == height) {
    if (i420) {
      for (int y = 0; y < h; ++y) detail::i420_row_to_rgba(image, y, pixels.data() + size_t(y) * width * 4);
      return;
    }
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
  if (top > 0) detail::fill_opaque_black(pixels.data(), size_t(top) * width);
  if (top + dh < height) detail::fill_opaque_black(pixels.data() + size_t(top + dh) * width * 4, size_t(height - top - dh) * width);
  if (left > 0 || left + dw < width) {
    const int right = width - left - dw;
    for (int y = 0; y < dh; ++y) {
      auto* row_out = pixels.data() + size_t(y + top) * width * 4;
      if (left > 0) detail::fill_opaque_black(row_out, size_t(left));
      if (right > 0) detail::fill_opaque_black(row_out + size_t(left + dw) * 4, size_t(right));
    }
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

  int cached_y[2] = {-1, -1};
  std::vector<uint16_t> h_rows[2] = {std::vector<uint16_t>(size_t(dw) * 4), std::vector<uint16_t>(size_t(dw) * 4)};
  std::vector<unsigned char> i420_scratch;
  if (i420) i420_scratch.resize(size_t(w) * 4);

  const auto get_h_row = [&](int sy, int preferred_slot) -> const uint16_t* {
    if (cached_y[0] == sy) return h_rows[0].data();
    if (cached_y[1] == sy) return h_rows[1].data();
    cached_y[preferred_slot] = sy;
    auto* dst = h_rows[preferred_slot].data();
    const unsigned char* src = nullptr;
    if (i420) {
      detail::i420_row_to_bgra(image, sy, i420_scratch.data());
      src = i420_scratch.data();
    } else {
      src = image.pixels + size_t(sy) * image.stride;
    }
#if defined(__SSE2__) || defined(_M_X64)
    const auto zero = _mm_setzero_si128();
    for (int x = 0; x < dw; ++x) {
      const auto col = columns[x];
      const auto* p0 = src + col.first * 4;
      __m128i pair;
      if (col.second == col.first + 1) {
        pair = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p0));
      } else {
        int32_t v0, v1;
        std::memcpy(&v0, p0, 4);
        std::memcpy(&v1, src + col.second * 4, 4);
        pair = _mm_set_epi32(0, 0, v1, v0);
      }
      const auto u16 = _mm_unpacklo_epi8(pair, zero);
      const auto w0 = _mm_set1_epi16(static_cast<short>(256 - col.weight));
      const auto w1 = _mm_set1_epi16(static_cast<short>(col.weight));
      const auto sum = _mm_add_epi16(_mm_mullo_epi16(u16, w0), _mm_mullo_epi16(_mm_srli_si128(u16, 8), w1));
      const auto rgba = _mm_shufflelo_epi16(sum, _MM_SHUFFLE(3, 0, 1, 2));
      _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + x * 4), rgba);
      dst[x * 4 + 3] = 65280;
    }
#else
    for (int x = 0; x < dw; ++x) {
      const auto col = columns[x];
      const int w0 = 256 - col.weight, w1 = col.weight;
      dst[x * 4 + 0] = static_cast<uint16_t>(src[col.first * 4 + 2] * w0 + src[col.second * 4 + 2] * w1);
      dst[x * 4 + 1] = static_cast<uint16_t>(src[col.first * 4 + 1] * w0 + src[col.second * 4 + 1] * w1);
      dst[x * 4 + 2] = static_cast<uint16_t>(src[col.first * 4 + 0] * w0 + src[col.second * 4 + 0] * w1);
      dst[x * 4 + 3] = 65280;
    }
#endif
    return dst;
  };

  for (int y = 0; y < dh; ++y) {
    const auto row = sample(y, h, dh);
    const auto* u0 = get_h_row(row.first, 0);
    const auto* u1 = (row.second == row.first) ? u0 : get_h_row(row.second, cached_y[0] == row.first ? 1 : 0);
    auto* output = pixels.data() + (size_t(y + top) * width + left) * 4;
    int x = 0;
#if defined(__SSE2__) || defined(_M_X64)
    const auto vw0 = _mm_set1_epi16(static_cast<short>(256 - row.weight));
    const auto vw1 = _mm_set1_epi16(static_cast<short>(row.weight));
    const auto round = _mm_set1_epi32(32768);
    for (; x + 2 <= dw; x += 2) {
      const auto v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(u0 + x * 4));
      const auto v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(u1 + x * 4));
      const auto p0_lo = _mm_mullo_epi16(v0, vw0), p0_hi = _mm_mulhi_epu16(v0, vw0);
      const auto p1_lo = _mm_mullo_epi16(v1, vw1), p1_hi = _mm_mulhi_epu16(v1, vw1);
      const auto s0 = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(_mm_unpacklo_epi16(p0_lo, p0_hi), _mm_unpacklo_epi16(p1_lo, p1_hi)), round), 16);
      const auto s1 = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(_mm_unpackhi_epi16(p0_lo, p0_hi), _mm_unpackhi_epi16(p1_lo, p1_hi)), round), 16);
      const auto packed16 = _mm_packs_epi32(s0, s1);
      const auto packed8 = _mm_packus_epi16(packed16, packed16);
      _mm_storel_epi64(reinterpret_cast<__m128i*>(output + x * 4), packed8);
    }
#endif
    const int wy0 = 256 - row.weight, wy1 = row.weight;
    for (; x < dw; ++x) {
      for (int c = 0; c < 3; ++c) {
        output[x * 4 + c] = static_cast<unsigned char>((uint32_t(u0[x * 4 + c]) * wy0 + uint32_t(u1[x * 4 + c]) * wy1 + 32768) >> 16);
      }
      output[x * 4 + 3] = 255;
    }
  }
}
}
