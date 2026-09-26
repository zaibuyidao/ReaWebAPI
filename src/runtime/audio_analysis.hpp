#pragma once
#include <cstddef>
#include <memory>
#include <vector>
namespace reaweb {
/* Worker-only DSP. PCM ingestion into its input queue is separate and realtime safe. */
class AudioAnalysis {
public:
  AudioAnalysis(unsigned rate, unsigned channels, unsigned fft_size);
  ~AudioAnalysis();
  void process(const float* interleaved, size_t frames);
  std::vector<float> meter();
  std::vector<float> spectrum() const;
  std::vector<float> waveform(unsigned points) const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
