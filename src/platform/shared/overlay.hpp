#pragma once
#include "core/core.hpp"
#include <cmath>

namespace reaweb {
struct MouseRegion { double x, y, width, height; };
inline std::string window_layer(const Json& value) {
  if (!value.is_string() || (value != "normal" && value != "overlay"))
    throw Error("INVALID_ARGUMENT", "layer must be normal or overlay");
  return value.get<std::string>();
}
inline std::string mouse_mode(const Json& value) {
  if (!value.is_string() || (value != "normal" && value != "passthrough" && value != "regions"))
    throw Error("INVALID_ARGUMENT", "mouseMode must be normal, passthrough or regions");
  return value.get<std::string>();
}
inline std::vector<MouseRegion> mouse_regions(const Json& value) {
  if (!value.is_array()) throw Error("INVALID_ARGUMENT", "mouseRegions must be an array");
  std::vector<MouseRegion> regions;
  for (const auto& item : value) {
    if (!item.is_object()) throw Error("INVALID_ARGUMENT", "Each mouse region must be a rectangle");
    for (const auto* key : {"x", "y", "width", "height"})
      if (!item.contains(key) || !item[key].is_number() || !std::isfinite(item[key].get<double>()))
        throw Error("INVALID_ARGUMENT", "Mouse region coordinates must be finite numbers");
    MouseRegion r{item["x"].get<double>(), item["y"].get<double>(), item["width"].get<double>(), item["height"].get<double>()};
    if (r.width <= 0 || r.height <= 0) throw Error("INVALID_ARGUMENT", "Mouse region dimensions must be positive");
    regions.push_back(r);
  }
  return regions;
}
struct OverlayOptions {
  std::string layer = "normal", mode = "normal";
  std::vector<MouseRegion> regions;
  bool floating_only() const { return layer != "normal" || mode != "normal"; }
  static OverlayOptions parse(const Json& options) {
    return {window_layer(options.value("layer", Json("normal"))),
      mouse_mode(options.value("mouseMode", Json("normal"))),
      mouse_regions(options.value("mouseRegions", Json::array()))};
  }
};
inline bool mouse_hit(const std::string& mode, const std::vector<MouseRegion>& regions, double x, double y) {
  if (mode == "normal") return true;
  if (mode == "regions") for (const auto& r : regions)
    if (x >= r.x && y >= r.y && x - r.x < r.width && y - r.y < r.height) return true;
  return false;
}
inline Json overlay_capabilities(const std::string& layer_reason, const std::string& mouse_reason, const char* backend) {
  return {{"layer", {{"supported", layer_reason.empty()}, {"reason", layer_reason}}},
    {"passthrough", {{"supported", mouse_reason.empty()}, {"reason", mouse_reason}}},
    {"regions", {{"supported", mouse_reason.empty()}, {"reason", mouse_reason}}},
    {"combined", {{"supported", layer_reason.empty() && mouse_reason.empty()},
      {"reason", !layer_reason.empty() ? layer_reason : mouse_reason}}},
    {"floatingOnly", true}, {"backend", backend}};
}
}
