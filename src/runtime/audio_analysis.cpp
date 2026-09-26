#include "runtime/audio_analysis.hpp"
#include <ebur128.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>

namespace reaweb {
namespace {
constexpr double pi = 3.14159265358979323846;
void fft(std::vector<std::complex<double>>& data) {
  const auto count = data.size();
  for (size_t i = 1, j = 0; i < count; ++i) {
    size_t bit = count >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit;
    if (i < j) std::swap(data[i], data[j]);
  }
  for (size_t length = 2; length <= count; length <<= 1) {
    const auto step = std::polar(1.0, -2 * pi / length);
    for (size_t start = 0; start < count; start += length) {
      std::complex<double> phase = 1;
      for (size_t j = 0; j < length / 2; ++j) {
        const auto a = data[start + j], b = data[start + j + length / 2] * phase;
        data[start + j] = a + b; data[start + j + length / 2] = a - b; phase *= step;
      }
    }
  }
}
}
struct AudioAnalysis::Impl {
  unsigned rate, channels, fft_size;
  size_t position = 0, total = 0, interval_frames = 0;
  std::vector<float> history, peak;
  std::vector<double> square;
  ebur128_state* loudness = nullptr;
  ~Impl() { if (loudness) ebur128_destroy(&loudness); }
};
AudioAnalysis::AudioAnalysis(unsigned rate, unsigned channels, unsigned fft_size) : impl_(std::make_unique<Impl>()) {
  if (!rate || !channels || channels > 32 || fft_size < 32 || fft_size > 32768 || (fft_size & (fft_size - 1)))
    throw std::invalid_argument("Invalid audio analysis dimensions");
  auto& state = *impl_; state.rate = rate; state.channels = channels; state.fft_size = fft_size;
  state.history.resize(fft_size * channels); state.peak.resize(channels); state.square.resize(channels);
  state.loudness = ebur128_init(channels, rate, EBUR128_MODE_S | EBUR128_MODE_I | EBUR128_MODE_HISTOGRAM);
  if (!state.loudness) throw std::runtime_error("Cannot allocate loudness analyzer");
}
AudioAnalysis::~AudioAnalysis() = default;
void AudioAnalysis::process(const float* samples, size_t frames) {
  auto& state = *impl_;
  for (size_t i = 0; i < frames; ++i) {
    for (unsigned channel = 0; channel < state.channels; ++channel) {
      const auto sample = samples[i * state.channels + channel];
      if (!std::isfinite(sample)) throw std::invalid_argument("Non-finite audio sample");
      state.history[state.position * state.channels + channel] = sample;
      state.peak[channel] = std::max(state.peak[channel], std::abs(sample));
      state.square[channel] += double(sample) * sample;
    }
    state.position = (state.position + 1) % state.fft_size;
  }
  state.total += frames; state.interval_frames += frames;
  if (ebur128_add_frames_float(state.loudness, samples, frames) != EBUR128_SUCCESS) throw std::runtime_error("Loudness analysis failed");
}
std::vector<float> AudioAnalysis::meter() {
  auto& state = *impl_; std::vector<float> result(2 * state.channels + 4);
  for (unsigned channel = 0; channel < state.channels; ++channel) {
    result[channel] = state.peak[channel];
    result[state.channels + channel] = state.interval_frames ? static_cast<float>(std::sqrt(state.square[channel] / state.interval_frames)) : 0;
  }
  double momentary = -INFINITY, shortterm = -INFINITY, integrated = -INFINITY;
  ebur128_loudness_momentary(state.loudness, &momentary); ebur128_loudness_shortterm(state.loudness, &shortterm);
  ebur128_loudness_global(state.loudness, &integrated);
  result[2 * state.channels] = static_cast<float>(momentary); result[2 * state.channels + 1] = static_cast<float>(shortterm);
  result[2 * state.channels + 2] = static_cast<float>(integrated); result[2 * state.channels + 3] = static_cast<float>(double(state.total) / state.rate);
  std::fill(state.peak.begin(), state.peak.end(), 0); std::fill(state.square.begin(), state.square.end(), 0); state.interval_frames = 0;
  return result;
}
std::vector<float> AudioAnalysis::spectrum() const {
  const auto& state = *impl_; const auto bins = state.fft_size / 2 + 1;
  std::vector<float> result(bins * state.channels); std::vector<std::complex<double>> values(state.fft_size);
  for (unsigned channel = 0; channel < state.channels; ++channel) {
    for (unsigned i = 0; i < state.fft_size; ++i)
      values[i] = state.history[((state.position + i) % state.fft_size) * state.channels + channel] * (0.5 - 0.5 * std::cos(2 * pi * i / state.fft_size));
    fft(values);
    for (unsigned i = 0; i < bins; ++i)
      result[i * state.channels + channel] = static_cast<float>(std::abs(values[i]) * ((i == 0 || i == bins - 1) ? 2 : 4) / state.fft_size);
  }
  return result;
}
std::vector<float> AudioAnalysis::waveform(unsigned points) const {
  const auto& state = *impl_; points = std::max(1u, std::min(points, state.fft_size));
  std::vector<float> result(points * state.channels * 2);
  for (unsigned point = 0; point < points; ++point) for (unsigned channel = 0; channel < state.channels; ++channel) {
    float low = INFINITY, high = -INFINITY;
    for (unsigned i = point * state.fft_size / points; i < (point + 1) * state.fft_size / points; ++i) {
      const auto sample = state.history[((state.position + i) % state.fft_size) * state.channels + channel];
      low = std::min(low, sample); high = std::max(high, sample);
    }
    result[(point * state.channels + channel) * 2] = low; result[(point * state.channels + channel) * 2 + 1] = high;
  }
  return result;
}
}
