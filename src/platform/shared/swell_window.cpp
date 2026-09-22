#include "platform/shared/swell_window.hpp"
#include <swell/swell-dlggen.h>
#include <algorithm>

static SWELL_DialogRegHelper reaweb_dialog(&SWELL_curmodule_dialogresource_head,
  [](HWND, int) {}, 101, SWELL_DLG_WS_RESIZABLE, "ReaWebAPI", 860, 640, 1.0, 1.0);

namespace reaweb {
INT_PTR SwellWindow::procedure(HWND window, UINT message, WPARAM command, LPARAM parameter) {
  if (message == WM_INITDIALOG) { SetWindowLong(window, GWL_USERDATA, parameter); return TRUE; }
  auto self = reinterpret_cast<SwellWindow*>(GetWindowLong(window, GWL_USERDATA));
  if (!self) return FALSE;
  if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(command) == IDCANCEL && !HIWORD(command) && !parameter)) {
    if (self->close_) self->close_(); else self->closed_ = true;
    return TRUE;
  }
  if (message == WM_DESTROY) { self->closed_ = true; self->window_ = nullptr; }
  if (message == WM_SETFOCUS && self->focus_) self->focus_();
  return FALSE;
}
SwellWindow::SwellWindow(const std::string& title, void* parent, std::function<void()> focus, std::function<void()> close)
  : owner_(static_cast<HWND>(parent)), focus_(std::move(focus)), close_(std::move(close)) {
  window_ = CreateDialogParam(nullptr, MAKEINTRESOURCE(101), static_cast<HWND>(parent), procedure, reinterpret_cast<LPARAM>(this));
  if (!window_) throw std::runtime_error("Cannot create the REAPER WebView container");
  SetWindowText(window_, title.c_str());
  ShowWindow(window_, SW_SHOW);
}
SwellWindow::~SwellWindow() {
  if (window_ && IsWindow(window_)) {
    SetWindowLong(window_, GWL_USERDATA, 0);
    DestroyWindow(window_);
  }
}
bool SwellWindow::closed() const { return closed_ || !window_ || !IsWindow(window_); }
void SwellWindow::prepare_dock() { GetWindowRect(window_, &floating_); }
void SwellWindow::restore_floating() {
  SetParent(window_, nullptr);
  SetWindowLong(window_, GWL_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner_));
  SetWindowPos(window_, nullptr, floating_.left, floating_.top,
    floating_.right - floating_.left, floating_.bottom - floating_.top, SWP_NOZORDER | SWP_NOACTIVATE);
  ShowWindow(window_, SW_SHOW);
  SetForegroundWindow(window_);
  restore_placement(placement());
}
void SwellWindow::focus() {
  if (closed()) return;
  ShowWindow(window_, SW_SHOW);
  SetForegroundWindow(window_);
  SetFocus(window_);
  if (focus_) focus_();
}
void SwellWindow::set_title(const std::string& title) { SetWindowText(window_, title.c_str()); }
void SwellWindow::set_visible(bool visible) { ShowWindow(window_, visible ? SW_SHOWNOACTIVATE : SW_HIDE); }
bool SwellWindow::visible() const { return !closed() && IsWindowVisible(window_); }
bool SwellWindow::focused() const {
  auto focus = GetFocus();
  return !closed() && (focus == window_ || IsChild(window_, focus));
}
Json SwellWindow::placement() const {
  if (!window_) return nullptr;
  RECT rect{};
  if (!GetWindowRect(window_, &rect)) return nullptr;
  return {{"x", rect.left}, {"y", rect.top}, {"width", rect.right - rect.left}, {"height", rect.bottom - rect.top}, {"maximized", false}};
}
void SwellWindow::restore_placement(const Json& value) {
  if (value.is_null()) return;
  RECT rect{value.at("x").get<int>(), value.at("y").get<int>(), 0, 0}, viewport{};
  const int width = value.at("width"), height = value.at("height");
  rect.right = rect.left + width; rect.bottom = rect.top + height;
  SWELL_GetViewPort(&viewport, &rect, true);
  const auto w = std::min(width, static_cast<int>(viewport.right - viewport.left));
  const auto h = std::min(height, static_cast<int>(viewport.bottom - viewport.top));
  rect.left = std::clamp(rect.left, viewport.left, viewport.right - w);
  rect.top = std::clamp(rect.top, viewport.top, viewport.bottom - h);
  rect.right = rect.left + w; rect.bottom = rect.top + h;
  floating_ = rect;
  SetWindowPos(window_, nullptr, rect.left, rect.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}
}
