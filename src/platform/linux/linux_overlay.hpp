#pragma once
#include "platform/shared/overlay.hpp"
#include <memory>

namespace reaweb {
class LinuxOverlay {
  struct State;
  std::unique_ptr<State> state_;
public:
  LinuxOverlay(const OverlayOptions&, std::function<void(std::string)>);
  ~LinuxOverlay();
  void attach(unsigned long host, unsigned long owner);
  void scale(double value);
  void mode(const std::string&);
  void regions(const std::vector<MouseRegion>&);
  bool layer_active() const;
  bool mouse_active() const;
  static Json capabilities();
};
}
