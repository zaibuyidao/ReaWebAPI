#include "runtime/audio_analysis.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (0)
int main() {
  try {
    reaweb::AudioAnalysis analysis(48000, 2, 2048);
    float samples[2048 * 2];
    for (int frame = 0; frame < 2048; ++frame) {
      samples[frame * 2] = static_cast<float>(0.5 * std::sin(2 * 3.141592653589793 * frame * 64 / 2048));
      samples[frame * 2 + 1] = samples[frame * 2];
    }
    for (int i = 0; i < 100; ++i) analysis.process(samples, 2048);
    auto meter = analysis.meter();
    CHECK(meter.size() == 8 && std::abs(meter[0] - 0.5) < 0.001);
    CHECK(std::abs(meter[2] - 0.5 / std::sqrt(2.0)) < 0.001);
    CHECK(std::isfinite(meter[4]) && std::isfinite(meter[5]) && std::isfinite(meter[6]));
    CHECK(meter[4] > -15 && meter[4] < -3);
    auto spectrum = analysis.spectrum();
    CHECK(spectrum.size() == 1025 * 2);
    CHECK(std::abs(spectrum[64 * 2] - 0.5) < 0.001 && spectrum[10] < 0.001);
    auto waveform = analysis.waveform(128);
    CHECK(waveform.size() == 128 * 4);
    for (size_t i = 0; i < waveform.size(); i += 2) CHECK(waveform[i] <= waveform[i + 1]);
    auto reset_meter = analysis.meter(); CHECK(reset_meter[0] == 0 && reset_meter[2] == 0);
    std::cout << "PCM peak/RMS/LUFS, FFT calibration and waveform envelopes passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
