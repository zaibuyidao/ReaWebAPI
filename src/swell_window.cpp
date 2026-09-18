#include "swell_window.hpp"
#include <swell/swell-dlggen.h>

static SWELL_DialogRegHelper reaweb_dialog(&SWELL_curmodule_dialogresource_head,
  [](HWND, int) {}, 101, SWELL_DLG_WS_RESIZABLE, "ReaWebAPI", 860, 640, 1.0, 1.0);

namespace reaweb {
INT_PTR SwellWindow::procedure(HWND window, UINT message, WPARAM, LPARAM parameter) {
  if (message == WM_INITDIALOG) { SetWindowLong(window, GWL_USERDATA, parameter); return TRUE; }
  auto self = reinterpret_cast<SwellWindow*>(GetWindowLong(window, GWL_USERDATA));
  if (!self) return FALSE;
  if (message == WM_CLOSE) { self->closed_ = true; return TRUE; }
  if (message == WM_DESTROY) { self->closed_ = true; self->window_ = nullptr; }
  if (message == WM_SETFOCUS && self->focus_) self->focus_();
  return FALSE;
}
SwellWindow::SwellWindow(const std::string& title, void* parent, std::function<void()> focus)
  : owner_(static_cast<HWND>(parent)), focus_(std::move(focus)) {
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
}
}
