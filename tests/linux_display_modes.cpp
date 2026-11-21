#include "platform/linux/display_modes.hpp"
#include <iostream>

int main() {
  XRRModeInfo modes[4]{};
  const unsigned long clock[] = {60000000, 144000000, 165000000, 240000000};
  for (int i = 0; i < 4; ++i) {
    modes[i].id = i + 1;
    modes[i].dotClock = clock[i];
    modes[i].hTotal = modes[i].vTotal = 1000;
  }
  XRRScreenResources resources{};
  resources.modes = modes;
  resources.nmode = 4;
  char name[] = "DP-1";
  RRMode supported[] = {2, 3, 1};
  XRROutputInfo output{};
  output.name = name;
  output.connection = RR_Connected;
  output.crtc = 1;
  output.modes = supported;
  output.nmode = 3;
  if (reaweb::highest_output_refresh_rate(resources, output) != 165) return 1;
  modes[2].modeFlags = RR_DoubleScan;
  if (reaweb::highest_output_refresh_rate(resources, output) != 144) return 1;
  modes[1].vTotal = 0;
  modes[0].modeFlags = RR_Interlace;
  if (reaweb::highest_output_refresh_rate(resources, output) != 120) return 1;
  output.nmode = 0;
  if (reaweb::highest_output_refresh_rate(resources, output) != 0) return 1;
  std::cout << "XRandR mode selection passed: 165 Hz, output scoping, scan flags, invalid and absent modes\n";
}
