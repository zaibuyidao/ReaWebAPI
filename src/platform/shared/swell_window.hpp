#pragma once
#include "platform/platform.hpp"
#include <swell/swell.h>

namespace reaweb {
class SwellWindow {
public:
  SwellWindow(const std::string& title, void* parent, std::function<void()> focus = {}, std::function<void()> close = {},
    std::function<void()> dock = {}, std::function<bool()> is_docked = {});
  ~SwellWindow();
  void* handle() const { return window_; }
  bool closed() const;
  void prepare_dock();
  void restore_floating();
  void focus();
  void set_title(const std::string& title);
  void tick();
  int content_top() const { return dock_ ? 30 : 0; }
  void set_visible(bool visible);
  bool visible() const;
  bool focused() const;
  Json placement() const;
  void restore_placement(const Json& value);
private:
  HWND window_ = nullptr;
  HWND owner_ = nullptr;
  RECT floating_{};
  bool closed_ = false;
  std::function<void()> focus_;
  std::function<void()> close_;
  std::function<void()> dock_;
  std::function<bool()> is_docked_;
  bool last_docked_ = false;
  static INT_PTR procedure(HWND, UINT, WPARAM, LPARAM);
};
}
