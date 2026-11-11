#pragma once
#include <cmath>

namespace reaweb {
struct VideoPositionClock {
  double position = 0, sampled_at = 0;
  int play = -1, changes = -1;
  bool initialized = false;

  bool update(double current, double now, int playing, double rate, int revision) {
    const bool moved = current != position;
    const double expected = position + ((play & 1) ? (now - sampled_at) * rate : 0);
    const bool changed = !initialized || playing != play || revision != changes ||
      (moved && std::abs(current - expected) > ((playing & 1) ? 0.15 : 0.000001));
    // Repeated host positions do not advance the reference clock for seek detection.
    if (!initialized || moved || changed) { position = current; sampled_at = now; }
    play = playing; changes = revision; initialized = true;
    return changed;
  }
};
}
