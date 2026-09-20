#pragma once
#include "core/core.hpp"
#include <algorithm>
#include <cmath>

namespace reaweb {
// Persist preferences, never visibility or a live debugging session.
struct DevToolsPreferences {
  bool floating = false;
  double width_ratio = 0.4;
  void restore(const Json& value) {
    if (!value.is_object()) return;
    auto mode = value.find("mode");
    if (mode != value.end() && mode->is_string()) {
      if (*mode == "floating") floating = true;
      else if (*mode == "embedded") floating = false;
    }
    auto ratio = value.find("widthRatio");
    if (ratio != value.end() && ratio->is_number()) {
      const auto number = ratio->get<double>();
      if (std::isfinite(number)) width_ratio = std::clamp(number, 0.2, 0.8);
    }
  }
  Json state() const { return {{"mode", floating ? "floating" : "embedded"}, {"widthRatio", width_ratio}}; }
};
}
