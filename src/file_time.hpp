#pragma once
#include <algorithm>
#include <string>

namespace reaweb {
// libc++ file-clock ticks can be 128-bit, which std::to_string cannot accept.
// Preserve every tick rather than narrowing the value or reducing its precision.
template<class Rep> std::string file_time_ticks(Rep ticks) {
  const bool negative = ticks < 0;
  std::string text;
  do {
    const int digit = static_cast<int>(ticks % 10);
    // Negate only the digit so the minimum signed value is also supported.
    text.push_back(static_cast<char>('0' + (digit < 0 ? -digit : digit)));
    ticks /= 10;
  } while (ticks != 0);
  if (negative) text.push_back('-');
  std::reverse(text.begin(), text.end());
  return text;
}
}
