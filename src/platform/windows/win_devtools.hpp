#pragma once
#include "platform/shared/devtools.hpp"
#include "platform/windows/devtools_keys.hpp"
#include <set>
#include <chrono>

namespace reaweb {
// Chromium owns the inspector. Only its host container moves on mode switches.
class WinDevTools {
  inline static WinDevTools* opening_ = nullptr;
  static constexpr const wchar_t* marker = L"ReaWebAPI.DevTools.Owner";
  static constexpr const wchar_t* panel_class = L"ReaWebAPI.DevTools.Panel";
  static constexpr const wchar_t* splitter_class = L"ReaWebAPI.DevTools.Splitter";
  HWND window_ = nullptr, owner_ = nullptr, original_owner_ = nullptr;
  HWND panel_ = nullptr, floating_ = nullptr, splitter_ = nullptr;
  HWND content_ = nullptr;
  HWND foreground_ = nullptr;
  LONG_PTR original_style_ = 0, original_exstyle_ = 0;
  RECT original_rect_{};
  UINT32 process_ = 0;
  bool requested_ = false, pending_ = false, hosted_ = false, dragging_ = false, positioning_ = false;
  std::set<HWND> before_;
  std::chrono::steady_clock::time_point deadline_;
  DevToolsPreferences prefs_;
  std::string error_, fallback_;
  std::function<void()> changed_, focus_page_;
  std::shared_ptr<DevToolsKeys> keys_ = DevToolsKeys::acquire();

  static int scale(HWND window, int value) { return MulDiv(value, GetDpiForWindow(window), 96); }
  static bool set_style(HWND window, int index, LONG_PTR value) {
    SetLastError(0); return SetWindowLongPtrW(window, index, value) || !GetLastError();
  }
  static bool parent(HWND window, HWND next) {
    SetLastError(0); return SetParent(window, next) || !GetLastError();
  }
  static void place(HWND window, int x, int y, int width, int height) {
    SetWindowPos(window, nullptr, x, y, std::max(0, width), std::max(0, height), SWP_NOZORDER | SWP_NOACTIVATE);
  }
  bool valid() const {
    DWORD process = 0;
    if (window_) GetWindowThreadProcessId(window_, &process);
    return window_ && process == process_ && GetPropW(window_, marker) == static_cast<const void*>(this);
  }
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
  void update_owner() {
    auto target = hosted_ ? floating_ : window_;
    if (!target) return;
    const auto root = GetAncestor(owner_, GA_ROOT);
    if (GetWindow(target, GW_OWNER) != root)
      SetWindowLongPtrW(target, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(root));
    auto foreground = GetForegroundWindow();
    if (foreground != foreground_ && requested_ && (prefs_.floating || !hosted_)) {
      DWORD process = 0; GetWindowThreadProcessId(foreground, &process);
      // Raise within REAPER without activation or a global topmost flag.
      if (process == GetCurrentProcessId() && IsWindowVisible(target) && !IsIconic(target))
        SetWindowPos(target, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    foreground_ = foreground;
  }
  void panel_layout() {
    if (!hosted_ || positioning_) return;
    positioning_ = true;
    if (prefs_.floating) {
      RECT rect{}; GetClientRect(floating_, &rect); place(panel_, 0, 0, rect.right, rect.bottom);
    }
    RECT rect{}; GetClientRect(panel_, &rect);
    if (valid()) {
      if (content_) {
        place(content_, 0, 0, rect.right, rect.bottom);
        const auto previous = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(content_));
        RECT content{}; GetClientRect(content_, &content);
        place(window_, 0, 0, content.right, content.bottom);
        if (previous) SetThreadDpiAwarenessContext(previous);
      } else place(window_, 0, 0, rect.right, rect.bottom);
    }
    positioning_ = false;
  }
  void focus_inspector() {
    if (!valid()) return;
    SetForegroundWindow(hosted_ ? GetAncestor(panel_, GA_ROOT) : window_);
    SetFocus(window_);
  }
  void hide(bool activate = true) {
    requested_ = false;
    if (valid()) ShowWindow(window_, SW_HIDE);
    ShowWindow(panel_, SW_HIDE); ShowWindow(floating_, SW_HIDE); ShowWindow(splitter_, SW_HIDE);
    changed_(); if (activate) focus_page_();
  }
  void present(bool activate = true) {
    if (!valid()) return;
    if (!requested_) { hide(activate); return; }
    update_owner();
    if (hosted_) {
      ShowWindow(panel_, SW_HIDE);
      auto container = prefs_.floating ? floating_ : owner_;
      if (GetParent(panel_) != container && !parent(panel_, container)) {
        prefs_.floating = GetParent(panel_) == floating_;
        error_ = "Could not move the DevTools container";
      }
      ShowWindow(floating_, prefs_.floating ? (IsIconic(floating_) ? SW_RESTORE : SW_SHOWNOACTIVATE) : SW_HIDE);
      ShowWindow(panel_, SW_SHOWNOACTIVATE);
      changed_(); panel_layout(); ShowWindow(window_, SW_SHOWNOACTIVATE);
    } else ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOWNOACTIVATE);
    if (activate) focus_inspector();
  }
  void restore_native() {
    if (!valid()) return;
    ShowWindow(window_, SW_HIDE);
    // Detach Chromium before destroying any host container.
    if ((GetWindowLongPtrW(window_, GWL_STYLE) & WS_CHILD) && !parent(window_, nullptr)) {
      error_ = "Could not detach the native DevTools window"; return;
    }
    set_style(window_, GWL_STYLE, original_style_ & ~WS_VISIBLE);
    set_style(window_, GWL_EXSTYLE, original_exstyle_);
    SetWindowLongPtrW(window_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(IsWindow(original_owner_) ? original_owner_ : nullptr));
    SetWindowPos(window_, nullptr, original_rect_.left, original_rect_.top,
      original_rect_.right - original_rect_.left, original_rect_.bottom - original_rect_.top,
      SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    hosted_ = false;
  }
  void attach() {
    original_owner_ = GetWindow(window_, GW_OWNER);
    original_style_ = GetWindowLongPtrW(window_, GWL_STYLE);
    original_exstyle_ = GetWindowLongPtrW(window_, GWL_EXSTYLE);
    GetWindowRect(window_, &original_rect_); fallback_.clear();
    if (content_) { DestroyWindow(content_); content_ = nullptr; }
    auto container = panel_;
    const auto inspector_dpi = GetWindowDpiAwarenessContext(window_);
    const auto host_dpi = GetWindowDpiAwarenessContext(panel_);
    if (!AreDpiAwarenessContextsEqual(inspector_dpi, host_dpi)) {
      // A local DPI island bridges PMv1/PMv2 without resetting Chromium's process DPI.
      if (GetAwarenessFromDpiAwarenessContext(inspector_dpi) == DPI_AWARENESS_PER_MONITOR_AWARE &&
          GetAwarenessFromDpiAwarenessContext(host_dpi) != DPI_AWARENESS_PER_MONITOR_AWARE) {
        fallback_ = "Per-monitor DevTools cannot be embedded in a system-DPI or DPI-unaware host"; return;
      }
      const auto previous = SetThreadDpiAwarenessContext(inspector_dpi);
      if (previous) {
        content_ = CreateWindowExW(0, panel_class, L"DevTools content", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
          0, 0, 0, 0, panel_, nullptr, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner_, GWLP_HINSTANCE)), this);
        SetThreadDpiAwarenessContext(previous);
      }
      if (!content_ || !AreDpiAwarenessContextsEqual(inspector_dpi, GetWindowDpiAwarenessContext(content_))) {
        if (content_) { DestroyWindow(content_); content_ = nullptr; }
        fallback_ = "Could not create a compatible DPI container for DevTools"; return;
      }
      container = content_;
    }
    ShowWindow(window_, SW_HIDE);
    const auto style = (original_style_ & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_VISIBLE)) | WS_CHILD;
    if (!set_style(window_, GWL_STYLE, style) ||
        !set_style(window_, GWL_EXSTYLE, original_exstyle_ & ~(WS_EX_APPWINDOW | WS_EX_TOPMOST | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE)) ||
        !parent(window_, container)) {
      fallback_ = "The WebView2 native DevTools window could not be hosted";
      restore_native(); return;
    }
    hosted_ = true;
    SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SendMessageW(panel_, WM_CHANGEUISTATE, MAKEWPARAM(UIS_INITIALIZE, 0), 0);
  }
  void lost() {
    window_ = nullptr; hosted_ = requested_ = false;
    ShowWindow(panel_, SW_HIDE); ShowWindow(floating_, SW_HIDE); ShowWindow(splitter_, SW_HIDE); changed_();
  }
  static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto self = reinterpret_cast<WinDevTools*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
      self = static_cast<WinDevTools*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (hwnd == self->splitter_) {
      if (msg == WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
      if (msg == WM_LBUTTONDOWN) { self->dragging_ = true; SetCapture(hwnd); return 0; }
      if (msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED) {
        self->dragging_ = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
      }
      if (msg == WM_MOUSEMOVE && self->dragging_) {
        POINT point{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        MapWindowPoints(hwnd, self->owner_, &point, 1);
        RECT rect{}; GetClientRect(self->owner_, &rect);
        const auto available = rect.right - scale(hwnd, 6);
        if (available > 0) self->prefs_.width_ratio = std::clamp(1.0 - static_cast<double>(point.x) / available, 0.2, 0.8);
        self->changed_(); return 0;
      }
    } else {
      if (msg == WM_CLOSE) { self->hide(); return 0; }
      if (msg == WM_SIZE) { self->panel_layout(); return 0; }
      if (msg == WM_SETFOCUS && self->requested_) { self->focus_inspector(); return 0; }
      if (msg == WM_DPICHANGED) {
        if (hwnd == self->floating_) {
          const auto& rect = *reinterpret_cast<RECT*>(lp);
          place(hwnd, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
        }
        self->panel_layout(); return 0;
      }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
public:
  WinDevTools(HWND owner, std::function<void()> changed, std::function<void()> focus_page)
    : owner_(owner), changed_(std::move(changed)), focus_page_(std::move(focus_page)) {
    auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    WNDCLASSW cls{}; cls.lpfnWndProc = proc; cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    for (auto name : {panel_class, splitter_class}) {
      cls.lpszClassName = name;
      if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Register DevTools container failed");
    }
    floating_ = CreateWindowExW(WS_EX_TOOLWINDOW, panel_class, L"ReaWebAPI DevTools", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
      CW_USEDEFAULT, CW_USEDEFAULT, scale(owner, 800), scale(owner, 600), GetAncestor(owner, GA_ROOT), nullptr, instance, this);
    using SetHosting = DPI_HOSTING_BEHAVIOR(WINAPI*)(DPI_HOSTING_BEHAVIOR);
    const auto set_hosting = reinterpret_cast<SetHosting>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiHostingBehavior"));
    const auto previous_hosting = set_hosting ? set_hosting(DPI_HOSTING_BEHAVIOR_MIXED) : DPI_HOSTING_BEHAVIOR_INVALID;
    panel_ = CreateWindowExW(WS_EX_CONTROLPARENT, panel_class, L"Developer Tools", WS_CHILD | WS_CLIPCHILDREN,
      0, 0, 0, 0, owner, nullptr, instance, this);
    if (previous_hosting != DPI_HOSTING_BEHAVIOR_INVALID) set_hosting(previous_hosting);
    splitter_ = CreateWindowExW(0, splitter_class, L"DevTools divider", WS_CHILD, 0, 0, 0, 0, owner, nullptr, instance, this);
    if (!floating_ || !panel_ || !splitter_) {
      if (panel_) DestroyWindow(panel_);
      if (floating_) DestroyWindow(floating_);
      if (splitter_) DestroyWindow(splitter_);
      throw std::runtime_error("Create DevTools container failed");
    }
    for (auto target : {owner_, floating_}) SetPropW(target, DevToolsKeys::target_property, reinterpret_cast<HANDLE>(owner_));
  }
  ~WinDevTools() {
    if (opening_ == this) opening_ = nullptr;
    if (valid()) {
      restore_native(); RemovePropW(window_, marker); RemovePropW(window_, DevToolsKeys::target_property);
    }
    RemovePropW(owner_, DevToolsKeys::target_property);
    DestroyWindow(panel_); DestroyWindow(floating_); DestroyWindow(splitter_);
  }
  void detach() { if (valid()) restore_native(); }
  RECT layout(RECT rect) {
    if (hosted_ && valid() && requested_ && !prefs_.floating) {
      const auto divider = std::min(rect.right, static_cast<LONG>(scale(owner_, 6)));
      const auto available = rect.right - divider;
      const auto width = static_cast<LONG>(available * prefs_.width_ratio);
      const auto left = available - width;
      place(splitter_, left, 0, divider, rect.bottom); place(panel_, left + divider, 0, width, rect.bottom);
      ShowWindow(splitter_, SW_SHOWNOACTIVATE); rect.right = left; panel_layout();
    } else ShowWindow(splitter_, SW_HIDE);
    return rect;
  }
  bool toggle() {
    if (window_ && !valid()) lost();
    if (requested_) hide(); else open();
    return requested_;
  }
  void open() {
    if (window_ && !valid()) lost();
    requested_ = true; if (valid()) present();
  }
  DevToolsMenuState menu_state() const {
    return {requested_, (valid() && !hosted_) || prefs_.floating, fallback_.empty()};
  }
  void perform(DevToolsAction action) {
    if (action == DevToolsAction::Open) open();
    else if (action == DevToolsAction::Hide) hide();
    else if (action == DevToolsAction::Float || fallback_.empty()) {
      prefs_.floating = action == DevToolsAction::Float;
      present(requested_);
    }
  }
  void tick(ICoreWebView2* webview) {
    if (window_ && !valid()) lost();
    if (valid()) { update_owner(); return; }
    if (!webview) return;
    if (!pending_) {
      if (!requested_ || opening_) return;
      if (FAILED(webview->get_BrowserProcessId(&process_)) || !process_) {
        requested_ = false; error_ = "WebView2 browser process is unavailable"; return;
      }
      before_ = candidates(); opening_ = this; pending_ = true; error_.clear();
      deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      if (FAILED(webview->OpenDevToolsWindow())) {
        requested_ = pending_ = false; opening_ = nullptr; error_ = "WebView2 could not open DevTools";
      }
      return;
    }
    auto found = candidates();
    for (auto window : before_) found.erase(window);
    // Only claim a new, unambiguous frame belonging to this browser process.
    if (found.size() == 1 && IsWindowVisible(*found.begin()) && GetWindowTextLengthW(*found.begin()) > 0 &&
        SetPropW(*found.begin(), marker, reinterpret_cast<HANDLE>(this))) {
      window_ = *found.begin();
      if (!keys_->available() || !SetPropW(window_, DevToolsKeys::target_property, reinterpret_cast<HANDLE>(owner_)))
        error_ = "DevTools shortcut is only available in the main WebView";
      pending_ = false; opening_ = nullptr; attach(); present();
    } else if (std::chrono::steady_clock::now() >= deadline_) {
      pending_ = requested_ = false; opening_ = nullptr;
      error_ = "Native DevTools window could not be identified safely; use its close button";
    }
  }
  Json state() const { return prefs_.state(); }
  void restore(const Json& value) { prefs_.restore(value); if (valid()) present(false); }
  Json diagnostics() const {
    auto state = prefs_.state();
    if (valid() && !hosted_) state["mode"] = "floating";
    state.update({{"visible", valid() && IsWindowVisible(window_) != FALSE}, {"pending", pending_ || (requested_ && !valid())},
      {"embeddedSupported", fallback_.empty()}, {"fallbackReason", fallback_}, {"lastError", error_}});
    return state;
  }
};
}
