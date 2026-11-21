#pragma once
#include "platform/platform.hpp"
#include <windows.h>
#include <algorithm>

namespace reaweb {
inline Json display_max_refresh_rate(HWND window) {
  MONITORINFOEXW monitor{};
  monitor.cbSize = sizeof(monitor);
  DWORD highest = 0;
  if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONULL), &monitor)) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsExW(monitor.szDevice, i, &mode, 0); ++i) {
      if ((mode.dmFields & DM_DISPLAYFREQUENCY) && mode.dmDisplayFrequency > 1)
        highest = std::max(highest, mode.dmDisplayFrequency);
    }
  }
  return highest ? Json(highest) : Json(nullptr);
}
}
