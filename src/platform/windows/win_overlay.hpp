#pragma once
#include "platform/shared/overlay.hpp"
#include <windows.h>
#include <map>

namespace reaweb {
class WinOverlay {
  HWND host_ = nullptr, owner_ = nullptr;
  bool mouse_ = false;
  inline static HHOOK shared_mouse_ = nullptr;
  HWINEVENTHOOK order_ = nullptr;
  OverlayOptions requested_;
  std::string applied_mode_ = "normal";
  bool pressed_ = false, routing_ = false, repairing_ = false;
  bool added_layered_ = false;
  std::function<double()> scale_;
  std::function<void(std::string)> error_;
  inline static std::map<HWND, WinOverlay*> windows_;
  static bool buttons() {
    for (int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
      if (GetAsyncKeyState(key) & 0x8000) return true;
    return false;
  }
  static LRESULT CALLBACK mouse_hook(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION) for (auto& item : windows_) {
      auto& self = *item.second;
      if (!self.mouse_) continue;
      try {
        // Low-level hooks run before hit testing and before asynchronous button state changes.
        const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
        if (!self.pressed_) self.route(reinterpret_cast<MSLLHOOKSTRUCT*>(data)->pt);
        if (down) self.pressed_ = true;
        if (message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP)
          PostMessageW(self.host_, apply_message, 0, 0);
      } catch (const std::exception& e) { self.error_(std::string("MOUSE_MODE_FAILED: ") + e.what()); }
    }
    return CallNextHookEx(nullptr, code, message, data);
  }
  static void CALLBACK order_hook(HWINEVENTHOOK hook, DWORD, HWND hwnd, LONG object, LONG, DWORD, DWORD) {
    if (object != OBJID_WINDOW || !hwnd || GetAncestor(hwnd, GA_ROOT) != hwnd) return;
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return;
    for (auto& item : windows_) if (item.second->order_ == hook) item.second->repair();
  }
  void enable_mouse() {
    if (mouse_) return;
    if (!shared_mouse_) shared_mouse_ = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, GetModuleHandleW(nullptr), 0);
    if (!shared_mouse_) throw Error("MOUSE_MODE_UNAVAILABLE", "Cannot install the native mouse routing hook: " + std::to_string(GetLastError()));
    mouse_ = true;
    pressed_ = buttons();
  }
  void transparent(bool value) {
    auto style = GetWindowLongPtrW(host_, GWL_EXSTYLE);
    if (value == routing_) return;
    auto next = value ? style | WS_EX_LAYERED | WS_EX_TRANSPARENT : style & ~LONG_PTR(WS_EX_TRANSPARENT);
    if (!value && added_layered_) next &= ~LONG_PTR(WS_EX_LAYERED);
    SetLastError(0);
    if (!SetWindowLongPtrW(host_, GWL_EXSTYLE, next) && GetLastError())
      throw Error("MOUSE_MODE_FAILED", "Cannot change the native mouse routing style");
    if (value && !(style & WS_EX_LAYERED)) {
      if (!SetLayeredWindowAttributes(host_, 0, 255, LWA_ALPHA)) {
        SetWindowLongPtrW(host_, GWL_EXSTYLE, style);
        throw Error("MOUSE_MODE_FAILED", "Cannot establish layered mouse routing");
      }
      added_layered_ = true;
    }
    if (!value) added_layered_ = false;
    routing_ = value;
  }
  void route(POINT point) {
    POINT client = point; ScreenToClient(host_, &client);
    RECT bounds{}; GetClientRect(host_, &bounds);
    const bool inside = PtInRect(&bounds, client) != FALSE;
    const double scale = std::max(0.01, scale_());
    const bool pass = inside && !mouse_hit(requested_.mode, requested_.regions, client.x / scale, client.y / scale);
    transparent(pass);
    applied_mode_ = requested_.mode;
  }
public:
  static constexpr UINT apply_message = WM_APP + 78;
  ~WinOverlay() {
    if (order_) UnhookWinEvent(order_);
    windows_.erase(host_);
    bool used = false;
    for (const auto& item : windows_) if (item.second->mouse_) { used = true; break; }
    if (!used && shared_mouse_) { UnhookWindowsHookEx(shared_mouse_); shared_mouse_ = nullptr; }
  }
  void attach(HWND host, HWND owner, const OverlayOptions& options, std::function<double()> scale,
      std::function<void(std::string)> error) {
    host_ = host; owner_ = owner; requested_ = options; scale_ = std::move(scale); error_ = std::move(error);
    windows_[host] = this;
    if (options.layer == "overlay") {
      if (!IsWindow(owner)) throw Error("LAYER_UNAVAILABLE", "An overlay requires the REAPER main window");
      order_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_OBJECT_LOCATIONCHANGE, nullptr, order_hook,
        GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
      if (!order_) throw Error("LAYER_FAILED", "Cannot observe native window ordering");
      repair();
    }
    if (options.mode != "normal") enable_mouse();
    synchronize();
  }
  HWND predecessor() const {
    auto previous = GetWindow(owner_, GW_HWNDPREV);
    while (previous && (previous == host_ || (windows_.count(previous) && windows_.at(previous)->order_)))
      previous = GetWindow(previous, GW_HWNDPREV);
    return previous ? previous : HWND_TOP;
  }
  void position(WINDOWPOS* position) {
    if (order_ && !(position->flags & SWP_NOZORDER)) {
      position->hwndInsertAfter = predecessor();
      position->flags |= SWP_NOOWNERZORDER;
    }
  }
  void repair() {
    if (!order_ || repairing_ || !IsWindow(host_) || !IsWindow(owner_) || !IsWindowVisible(host_)) return;
    auto below = GetWindow(host_, GW_HWNDNEXT);
    while (below && below != owner_ && windows_.count(below) && windows_.at(below)->order_)
      below = GetWindow(below, GW_HWNDNEXT);
    if (below == owner_) return;
    const auto previous = predecessor();
    if (GetWindow(host_, GW_HWNDPREV) == previous || (previous == HWND_TOP && !GetWindow(host_, GW_HWNDPREV))) return;
    repairing_ = true;
    if (!SetWindowPos(host_, previous, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER))
      error_("LAYER_FAILED: Cannot restore overlay order");
    repairing_ = false;
  }
  void synchronize() {
    if (pressed_ && buttons()) return;
    pressed_ = false;
    POINT point{}; GetCursorPos(&point); route(point);
  }
  void mode(const std::string& value) {
    if (value != "normal") enable_mouse();
    const auto previous = requested_; requested_.mode = value;
    try { synchronize(); } catch (...) { requested_ = previous; throw; }
  }
  void regions(const std::vector<MouseRegion>& value) {
    const auto previous = requested_; requested_.regions = value;
    try { synchronize(); } catch (...) { requested_ = previous; throw; }
  }
  bool layer_active() const { return order_ != nullptr; }
  bool mouse_active() const { return requested_.mode == applied_mode_; }
};
}
