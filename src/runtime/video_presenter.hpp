#pragma once
#include "core/core.hpp"
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <functional>
#include <memory>
#include <limits>

namespace reaweb {
struct VideoPresentationFrame {
  const unsigned char* data = nullptr;
  size_t bytes = 0;
  uint64_t sequence = 0;
  double timestamp = 0;
  bool discontinuity = false;
};

class VideoPresentationSource {
public:
  virtual ~VideoPresentationSource() = default;
  // Called once per source frame by the transport thread. Data is borrowed for
  // this call. Copy into bounded storage without waiting for GPU presentation.
  virtual void submit(const VideoPresentationFrame&) = 0;
};

class VideoPresenter {
public:
  virtual ~VideoPresenter() = default;
  virtual std::shared_ptr<VideoPresentationSource> source() const = 0;
  virtual void layout(const Json&) = 0;
  virtual Json diagnostics() const = 0;
  virtual bool closed() const = 0;
  // May run on either the main or transport thread. Invalidate pending work
  // immediately, then arrange native view cleanup without blocking the caller.
  virtual void invalidate() noexcept = 0;
};
using VideoPresenterFactory = std::function<std::shared_ptr<VideoPresenter>(const Json&)>;

inline void validate_video_surface_layout(const Json& value) {
  if (!value.is_object() || !value.contains("rect") || !value.contains("clip"))
    throw Error("INVALID_ARGUMENT", "Video layout requires rect and clip");
  for (const auto& field : value.items())
    if (field.key() != "rect" && field.key() != "clip" && field.key() != "preserveAspect")
      throw Error("INVALID_ARGUMENT", "Unknown video layout field");
  if (value.contains("preserveAspect") && !value["preserveAspect"].is_boolean())
    throw Error("INVALID_ARGUMENT", "preserveAspect must be boolean");
  for (const auto* key : {"rect", "clip"}) {
    const auto& rect = value[key];
    if (!rect.is_object() || rect.size() != 4)
      throw Error("INVALID_ARGUMENT", "Video rectangles require x, y, width and height");
    for (const auto* field : {"x", "y", "width", "height"}) {
      if (!rect.contains(field) || !rect[field].is_number() ||
          !std::isfinite(rect[field].get<double>()) ||
          std::abs(rect[field].get<double>()) > (std::numeric_limits<float>::max)() ||
          ((field[0] == 'w' || field[0] == 'h') && rect[field].get<double>() < 0))
        throw Error("INVALID_ARGUMENT", "Invalid video rectangle");
    }
  }
}
}
