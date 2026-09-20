#pragma once
#include "platform/shared/devtools.hpp"
#include "platform/windows/devtools_keys.hpp"
#include <set>
#include <chrono>

namespace reaweb {
// Associates native floating DevTools with its WebView for visibility and ownership.
class WinDevTools {
  inline static WinDevTools* opening_ = nullptr;
  static constexpr const wchar_t* marker = L"ReaWebAPI.DevTools.Owner";
  HWND window_ = nullptr, owner_ = nullptr, original_owner_ = nullptr;
  UINT32 process_ = 0;
  bool requested_ = false, pending_ = false;
  std::set<HWND> before_;
  std::chrono::steady_clock::time_point deadline_;
  DevToolsPreferences prefs_;
  std::string error_;
  std::shared_ptr<DevToolsKeys> keys_ = DevToolsKeys::acquire();

  std::set<HWND> candidates() const {
    struct Search { DWORD process; std::set<HWND> windows; } search{process_, {}};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
      auto& search = *reinterpret_cast<Search*>(parameter);
      DWORD process = 0; GetWindowThreadProcessId(window, &process);
      wchar_t name[128]{}; GetClassNameW(window, name, 128);
      const auto style = GetWindowLongPtrW(window, GWL_STYLE);
      if (process == search.process && !wcscmp(name, L"Chrome_WidgetWin_1") &&
          !(style & WS_CHILD) && (style & (WS_CAPTION | WS_THICKFRAME)) == (WS_CAPTION | WS_THICKFRAME) &&
          !GetPropW(window, marker)) search.windows.insert(window);
      return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.windows;
  }
  bool valid() const {
    DWORD process = 0;
    if (window_) GetWindowThreadProcessId(window_, &process);
    return window_ && process == process_ && GetPropW(window_, marker) == static_cast<const void*>(this);
  }
  void update_owner() {
    const auto root = GetAncestor(owner_, GA_ROOT);
    if (GetWindow(window_, GW_OWNER) != root)
      SetWindowLongPtrW(window_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(root));
  }
  void show(bool visible) {
    if (!valid()) return;
    if (visible) {
      // Owned, non-modal: stay above this REAPER root without global topmost.
      update_owner();
      ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
      SetForegroundWindow(window_);
    } else ShowWindow(window_, SW_HIDE);
  }
public:
  explicit WinDevTools(HWND owner) : owner_(owner) { prefs_.floating = true; }
  ~WinDevTools() {
    if (opening_ == this) opening_ = nullptr;
    if (valid()) {
      // The controller closes its Inspector. Restore ownership first so that
      // destroying our HWND cannot also destroy Chromium's cross-process HWND.
      SetWindowLongPtrW(window_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(IsWindow(original_owner_) ? original_owner_ : nullptr));
      RemovePropW(window_, marker);
      RemovePropW(window_, DevToolsKeys::target_property);
    }
  }
  bool toggle() {
    if (window_ && !valid()) { window_ = nullptr; requested_ = false; }
    if (valid()) { requested_ = !IsWindowVisible(window_); show(requested_); }
    else requested_ = !requested_;
    return requested_;
  }
  void open() { requested_ = true; if (valid()) show(true); else if (window_) window_ = nullptr; }
  void tick(ICoreWebView2* webview) {
    if (window_ && !valid()) { window_ = nullptr; requested_ = false; }
    if (valid()) {
      // Docking the main WebView can change the native root owner.
      update_owner();
      return;
    }
    if (!webview) return;
    if (!pending_) {
      if (!requested_ || opening_) return;
      if (FAILED(webview->get_BrowserProcessId(&process_)) || !process_) { requested_ = false; return; }
      before_ = candidates(); opening_ = this; pending_ = true; error_.clear();
      deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      if (FAILED(webview->OpenDevToolsWindow())) {
        requested_ = pending_ = false; opening_ = nullptr; error_ = "WebView2 could not open DevTools";
      }
      return;
    }
    auto found = candidates();
    for (auto window : before_) found.erase(window);
    // Require a single candidate to avoid claiming another window. A title
    // excludes blank startup frames that can race rapid close/reopen.
    if (found.size() == 1 && IsWindowVisible(*found.begin()) && GetWindowTextLengthW(*found.begin()) > 0 &&
        SetPropW(*found.begin(), marker, reinterpret_cast<HANDLE>(this))) {
      window_ = *found.begin(); original_owner_ = GetWindow(window_, GW_OWNER);
      if (!keys_->available() || !SetPropW(window_, DevToolsKeys::target_property, reinterpret_cast<HANDLE>(owner_)))
        error_ = "DevTools shortcut is only available in the main WebView";
      pending_ = false; opening_ = nullptr; show(requested_);
    } else if (std::chrono::steady_clock::now() >= deadline_) {
      pending_ = requested_ = false; opening_ = nullptr;
      error_ = "Native DevTools window could not be identified safely; use its close button";
    }
  }
  Json state() const { return prefs_.state(); }
  void restore(const Json& value) { prefs_.restore(value); prefs_.floating = true; }
  Json diagnostics() const {
    auto state = prefs_.state();
    state.update({{"visible", valid() && IsWindowVisible(window_) != FALSE}, {"pending", pending_ || (requested_ && !valid())},
      {"embeddedSupported", false}, {"fallbackReason", "Embedded DevTools is not implemented by this backend"}, {"lastError", error_}});
    return state;
  }
};
}
