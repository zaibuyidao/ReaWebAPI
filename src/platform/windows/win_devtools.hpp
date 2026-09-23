#pragma once
#include "platform/shared/devtools.hpp"
#include "platform/windows/devtools_keys.hpp"
#include <set>
#include <chrono>

namespace reaweb {
// Chromium owns the inspector session in both embedded and native floating modes.
class WinDevTools {
  inline static WinDevTools* opening_ = nullptr;
  static constexpr const wchar_t* marker = L"ReaWebAPI.DevTools.Owner";
  static constexpr const wchar_t* panel_class = L"ReaWebAPI.DevTools.Panel";
  static constexpr const wchar_t* splitter_class = L"ReaWebAPI.DevTools.Splitter";
  static constexpr UINT focus_inspector_message = WM_APP + 74;
  HWND window_ = nullptr, owner_ = nullptr, original_owner_ = nullptr;
  HWND panel_ = nullptr, splitter_ = nullptr;
  HWND content_ = nullptr, renderer_ = nullptr;
  HWND foreground_ = nullptr;
  LONG_PTR original_style_ = 0, original_exstyle_ = 0;
  WINDOWPLACEMENT native_placement_{sizeof(WINDOWPLACEMENT)};
  UINT32 process_ = 0;
  bool requested_ = false, pending_ = false, hosted_ = false, dragging_ = false, positioning_ = false;
  bool maximize_on_show_ = false;
  std::set<HWND> before_;
  std::chrono::steady_clock::time_point deadline_;
  DevToolsPreferences prefs_;
  std::string error_, fallback_;
  std::function<void()> changed_, focus_page_;
  std::shared_ptr<DevToolsKeys> keys_ = DevToolsKeys::acquire();

  static bool set_style(HWND window, int index, LONG_PTR value) {
    SetLastError(0); return SetWindowLongPtrW(window, index, value) || !GetLastError();
  }
  static bool parent(HWND window, HWND next) {
    SetLastError(0); return SetParent(window, next) || !GetLastError();
  }
  static void place(HWND window, int x, int y, int width, int height) {
    SetWindowPos(window, nullptr, x, y, std::max(0, width), std::max(0, height), SWP_NOZORDER | SWP_NOACTIVATE);
  }
  static HWND renderer(HWND window) {
    struct Search { HWND found = nullptr; int count = 0; } search;
    EnumChildWindows(window, [](HWND child, LPARAM parameter) -> BOOL {
      auto& search = *reinterpret_cast<Search*>(parameter);
      wchar_t name[128]{}; GetClassNameW(child, name, 128);
      RECT rect{}; GetClientRect(child, &rect);
      if (!wcscmp(name, L"Chrome_RenderWidgetHostHWND") &&
          (GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) && rect.right > 0 && rect.bottom > 0) {
        search.found = child; ++search.count;
      }
      return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.count == 1 ? search.found : nullptr;
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
    auto target = window_;
    if (!target || hosted_) return;
    const auto root = GetAncestor(owner_, GA_ROOT);
    if (GetWindow(target, GW_OWNER) != root)
      SetWindowLongPtrW(target, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(root));
    auto foreground = GetForegroundWindow();
    if (foreground != foreground_ && requested_) {
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
    RECT rect{}; GetClientRect(panel_, &rect);
    if (valid()) {
      if (content_) place(content_, 0, 0, rect.right, rect.bottom);
      const auto previous = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(window_));
      auto container = content_ ? content_ : panel_;
      GetClientRect(container, &rect);
      RECT bounds{}, client{}, view{};
      GetWindowRect(window_, &bounds); GetClientRect(window_, &client);
      MapWindowPoints(window_, nullptr, reinterpret_cast<POINT*>(&client), 2);
      if (!IsChild(window_, renderer_)) renderer_ = renderer(window_);
      if (renderer_ && GetWindowRect(renderer_, &view)) {
        // Chromium draws a client-area caption even without WS_CAPTION. Clip it at
        // the renderer origin; use client edges so asynchronous renderer resize
        // cannot turn stale trailing bounds into growing margins.
        const auto left = std::max(0L, view.left - bounds.left);
        const auto top = std::max(0L, view.top - bounds.top);
        const auto right = std::max(0L, bounds.right - client.right);
        const auto bottom = std::max(0L, bounds.bottom - client.bottom);
        const auto width = rect.right + left + right, height = rect.bottom + top + bottom;
        POINT origin{bounds.left, bounds.top}; ScreenToClient(container, &origin);
        if (origin.x != -left || origin.y != -top || bounds.right - bounds.left != width || bounds.bottom - bounds.top != height)
          place(window_, -left, -top, width, height);
      }
      if (previous) SetThreadDpiAwarenessContext(previous);
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
    ShowWindow(panel_, SW_HIDE); ShowWindow(splitter_, SW_HIDE);
    changed_(); if (activate) focus_page_();
  }
  void present(bool activate = true) {
    if (!valid()) return;
    if (prefs_.floating && hosted_) restore_native();
    else if (!prefs_.floating && !hosted_ && fallback_.empty()) attach();
    if (!requested_) { hide(activate); return; }
    update_owner();
    if (hosted_) {
      ShowWindow(panel_, SW_SHOWNOACTIVATE);
      changed_(); panel_layout(); ShowWindow(window_, SW_SHOWNOACTIVATE);
    } else {
      ShowWindow(panel_, SW_HIDE); changed_();
      ShowWindow(window_, IsIconic(window_) ? SW_RESTORE :
        (maximize_on_show_ ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE));
      maximize_on_show_ = false;
    }
    if (activate) focus_inspector();
  }
  void restore_native() {
    if (!valid() || !hosted_) return;
    ShowWindow(window_, SW_HIDE);
    // Detach Chromium before destroying any host container.
    if ((GetWindowLongPtrW(window_, GWL_STYLE) & WS_CHILD) && !parent(window_, nullptr)) {
      error_ = "Could not detach the native DevTools window"; return;
    }
    set_style(window_, GWL_STYLE, original_style_ & ~(WS_VISIBLE | WS_MINIMIZE | WS_MAXIMIZE));
    set_style(window_, GWL_EXSTYLE, original_exstyle_);
    SetWindowLongPtrW(window_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(IsWindow(original_owner_) ? original_owner_ : nullptr));
    auto placement = native_placement_; placement.showCmd = SW_HIDE;
    SetWindowPlacement(window_, &placement);
    SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    maximize_on_show_ = native_placement_.showCmd == SW_SHOWMAXIMIZED;
    hosted_ = false;
  }
  void attach() {
    renderer_ = renderer(window_);
    if (!renderer_) { fallback_ = "The DevTools content bounds are unavailable"; return; }
    original_style_ = GetWindowLongPtrW(window_, GWL_STYLE);
    original_exstyle_ = GetWindowLongPtrW(window_, GWL_EXSTYLE);
    GetWindowPlacement(window_, &native_placement_); fallback_.clear();
    if (IsZoomed(window_) || maximize_on_show_) native_placement_.showCmd = SW_SHOWMAXIMIZED;
    maximize_on_show_ = false;
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
        content_ = CreateWindowExW(WS_EX_CONTROLPARENT, panel_class, L"DevTools content", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
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
    const auto style = (original_style_ & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_VISIBLE | WS_MINIMIZE | WS_MAXIMIZE)) | WS_CHILD;
    hosted_ = true;
    if (!set_style(window_, GWL_STYLE, style) ||
        !set_style(window_, GWL_EXSTYLE, original_exstyle_ & ~(WS_EX_APPWINDOW | WS_EX_TOPMOST | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE)) ||
        !parent(window_, container)) {
      fallback_ = "The WebView2 native DevTools window could not be hosted";
      restore_native(); return;
    }
    SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SendMessageW(panel_, WM_CHANGEUISTATE, MAKEWPARAM(UIS_INITIALIZE, 0), 0);
  }
  void lost() {
    window_ = renderer_ = nullptr; hosted_ = requested_ = false;
    maximize_on_show_ = false;
    fallback_.clear();
    ShowWindow(panel_, SW_HIDE); ShowWindow(splitter_, SW_HIDE); changed_();
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
        const auto available = rect.right - 1;
        if (available > 0) self->prefs_.width_ratio = std::clamp(1.0 - static_cast<double>(point.x) / available, 0.2, 0.8);
        self->changed_(); return 0;
      }
    } else {
      if (msg == WM_CLOSE) { self->hide(); return 0; }
      if (msg == WM_SIZE) { self->panel_layout(); return 0; }
      if (msg == WM_SETFOCUS && self->requested_) {
        // Do not reactivate an inspector from inside a cross-process focus change.
        PostMessageW(hwnd, focus_inspector_message, 0, 0); return 0;
      }
      if (msg == focus_inspector_message) {
        if (self->requested_ && GetFocus() == hwnd &&
            GetForegroundWindow() == GetAncestor(hwnd, GA_ROOT)) self->focus_inspector();
        return 0;
      }
      if (msg == WM_DPICHANGED) {
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
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    for (auto name : {panel_class, splitter_class}) {
      cls.lpszClassName = name;
      cls.hbrBackground = GetSysColorBrush(name == splitter_class ? COLOR_WINDOWFRAME : COLOR_BTNFACE);
      if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Register DevTools container failed");
    }
    using SetHosting = DPI_HOSTING_BEHAVIOR(WINAPI*)(DPI_HOSTING_BEHAVIOR);
    const auto set_hosting = reinterpret_cast<SetHosting>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiHostingBehavior"));
    const auto previous_hosting = set_hosting ? set_hosting(DPI_HOSTING_BEHAVIOR_MIXED) : DPI_HOSTING_BEHAVIOR_INVALID;
    panel_ = CreateWindowExW(WS_EX_CONTROLPARENT, panel_class, L"Developer Tools", WS_CHILD | WS_CLIPCHILDREN,
      0, 0, 0, 0, owner, nullptr, instance, this);
    if (previous_hosting != DPI_HOSTING_BEHAVIOR_INVALID) set_hosting(previous_hosting);
    splitter_ = CreateWindowExW(0, splitter_class, L"DevTools divider", WS_CHILD, 0, 0, 0, 0, owner, nullptr, instance, this);
    if (!panel_ || !splitter_) {
      if (panel_) DestroyWindow(panel_);
      if (splitter_) DestroyWindow(splitter_);
      throw std::runtime_error("Create DevTools container failed");
    }
    SetPropW(owner_, DevToolsKeys::target_property, reinterpret_cast<HANDLE>(owner_));
  }
  ~WinDevTools() {
    if (opening_ == this) opening_ = nullptr;
    if (valid()) {
      detach(); RemovePropW(window_, marker); RemovePropW(window_, DevToolsKeys::target_property);
    }
    RemovePropW(owner_, DevToolsKeys::target_property);
    DestroyWindow(panel_); DestroyWindow(splitter_);
  }
  void detach() {
    if (!valid()) return;
    restore_native(); ShowWindow(window_, SW_HIDE);
    if (!hosted_) SetWindowLongPtrW(window_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(IsWindow(original_owner_) ? original_owner_ : nullptr));
  }
  RECT layout(RECT rect) {
    if (hosted_ && valid() && requested_) {
      const auto divider = std::min(rect.right, 1L);
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
    if (valid()) { update_owner(); panel_layout(); return; }
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
    const bool expired = std::chrono::steady_clock::now() >= deadline_;
    // Only claim a new, unambiguous frame belonging to this browser process.
    if (found.size() == 1 && IsWindowVisible(*found.begin()) && GetWindowTextLengthW(*found.begin()) > 0 &&
        (prefs_.floating || renderer(*found.begin()) || expired) &&
        SetPropW(*found.begin(), marker, reinterpret_cast<HANDLE>(this))) {
      window_ = *found.begin();
      original_owner_ = GetWindow(window_, GW_OWNER);
      renderer_ = renderer(window_);
      GetWindowPlacement(window_, &native_placement_);
      if (!keys_->available() || !SetPropW(window_, DevToolsKeys::target_property, reinterpret_cast<HANDLE>(owner_)))
        error_ = "DevTools shortcut is only available in the main WebView";
      pending_ = false; opening_ = nullptr; present();
    } else if (expired) {
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
