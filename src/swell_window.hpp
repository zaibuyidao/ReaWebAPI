#pragma once
#include "platform.hpp"
#include <swell/swell.h>

namespace reaweb {
class SwellWindow {
public:
  SwellWindow(const std::string& title, void* parent, std::function<void()> focus = {});
  ~SwellWindow();
  void* handle() const { return window_; }
  bool closed() const;
  void prepare_dock();
  void restore_floating();
  void focus();
  void set_title(const std::string& title);
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
  static INT_PTR procedure(HWND, UINT, WPARAM, LPARAM);
};
}
