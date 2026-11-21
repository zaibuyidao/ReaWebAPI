#pragma once
#include <X11/extensions/Xrandr.h>
#include <algorithm>
#include <cstring>

namespace reaweb {
inline double highest_output_refresh_rate(const XRRScreenResources& resources, const XRROutputInfo& output) {
  if (!output.crtc || output.connection != RR_Connected || !output.nmode ||
      std::strncmp(output.name, "XWAYLAND", 8) == 0) return 0;
  double highest = 0;
  for (int i = 0; i < resources.nmode; ++i) {
    const auto& mode = resources.modes[i];
    if (!mode.hTotal || !mode.vTotal) continue;
    const auto end = output.modes + output.nmode;
    if (std::find(output.modes, end, mode.id) == end) continue;
    double hz = double(mode.dotClock) / mode.hTotal / mode.vTotal;
    if (mode.modeFlags & RR_DoubleScan) hz /= 2;
    if (mode.modeFlags & RR_Interlace) hz *= 2;
    highest = std::max(highest, hz);
  }
  return highest;
}
}
