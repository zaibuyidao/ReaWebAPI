#include "platform.hpp"
#include <windows.h>
#include <shellapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <vector>
#include <algorithm>
#include <cstring>

namespace reaweb {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
namespace {
std::wstring wide(const std::string& value) {
  const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (!size && !value.empty()) throw std::runtime_error("Invalid UTF-8 text");
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
  return result;
}
std::string utf8(const wchar_t* value) {
  if (!value) return {};
  const auto size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  std::string text(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), size, nullptr, nullptr);
  if (!text.empty()) text.pop_back();
  return text;
}
void check(HRESULT hr, const char* operation) {
  if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " failed (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ")");
}
constexpr wchar_t window_class[] = L"ReaWebAPI.Window";

class WinWindow final : public Window, public std::enable_shared_from_this<WinWindow> {
  WindowOptions options_;
  std::string uri_;
  HWND hwnd_ = nullptr;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> webview_;
  bool closed_ = false, want_devtools_ = false;
  RECT floating_rect_{};
  bool maximized_ = false;
  std::string browser_version_;
  bool visible_ = true;
public:
  static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto self = reinterpret_cast<WinWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
      self = static_cast<WinWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      self->hwnd_ = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) {
      if (msg == WM_SIZE && self->controller_) {
        RECT rect{}; GetClientRect(hwnd, &rect); self->controller_->put_Bounds(rect);
      } else if (msg == WM_MOVE && self->controller_) {
        self->controller_->NotifyParentWindowPositionChanged();
      } else if (msg == WM_SETFOCUS && self->controller_) {
        self->controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      } else if (msg == WM_CLOSE) {
        self->closed_ = true; return 0;
      } else if (msg == WM_NCDESTROY) {
        self->closed_ = true; self->hwnd_ = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  WinWindow(WindowOptions options, HINSTANCE instance) : options_(std::move(options)), uri_(options_.url.empty() ? file_uri(options_.entry) : options_.url) {
    hwnd_ = CreateWindowExW(0, window_class, wide(options_.title).c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
      CW_USEDEFAULT, CW_USEDEFAULT, 860, 640, static_cast<HWND>(options_.parent), nullptr, instance, this);
    if (!hwnd_) throw std::runtime_error("CreateWindowEx failed");
    // A REAPER-owned floating window stays above its owner when focus returns to REAPER.
    SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(options_.parent));
    ShowWindow(hwnd_, SW_SHOW);
  }
  ~WinWindow() override {
    closed_ = true;
    if (controller_) controller_->Close();
    webview_.Reset(); controller_.Reset();
    if (hwnd_) DestroyWindow(hwnd_);
  }
  void fail(const std::string& error) { closed_ = true; options_.on_error(error); }
  void initialize(ICoreWebView2Environment* environment) {
    if (closed_) return;
    LPWSTR version = nullptr;
    if (SUCCEEDED(environment->get_BrowserVersionString(&version))) browser_version_ = utf8(version);
    CoTaskMemFree(version);
    auto weak = weak_from_this();
    const auto hr = environment->CreateCoreWebView2Controller(hwnd_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
      [weak](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
        auto self = weak.lock();
        if (!self || self->closed_) { if (controller) controller->Close(); return S_OK; }
        try {
          check(result, "CreateCoreWebView2Controller");
          if (!controller) throw std::runtime_error("WebView2 controller is unavailable");
          self->controller_ = controller;
          check(controller->get_CoreWebView2(&self->webview_), "get_CoreWebView2");
          self->configure();
        } catch (const std::exception& e) { self->fail(e.what()); }
        return S_OK;
      }).Get());
    check(hr, "CreateCoreWebView2Controller");
  }
  void configure() {
    RECT rect{}; GetClientRect(hwnd_, &rect); controller_->put_Bounds(rect);
    ComPtr<ICoreWebView2Settings> settings;
    check(webview_->get_Settings(&settings), "get_Settings");
    settings->put_AreDevToolsEnabled(TRUE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_AreDefaultScriptDialogsEnabled(FALSE);
    ComPtr<ICoreWebView2Settings3> settings3;
    if (SUCCEEDED(settings.As(&settings3))) settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    EventRegistrationToken token{};
    auto weak = weak_from_this();
    check(webview_->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
      [weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
        if (auto self = weak.lock(); self && !self->closed_) {
          LPWSTR source = nullptr, message = nullptr;
          args->get_Source(&source);
          if (same_document(utf8(source), self->uri_) && SUCCEEDED(args->TryGetWebMessageAsString(&message)))
            self->options_.on_message(utf8(message));
          CoTaskMemFree(source); CoTaskMemFree(message);
        }
        return S_OK;
      }).Get(), &token), "add_WebMessageReceived");
    check(webview_->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
      [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
        LPWSTR uri = nullptr; args->get_Uri(&uri);
        auto self = weak.lock();
        if (!self || !same_document(utf8(uri), self->uri_)) args->put_Cancel(TRUE);
        else if (self->options_.on_navigation) self->options_.on_navigation();
        CoTaskMemFree(uri); return S_OK;
      }).Get(), &token), "add_NavigationStarting");
    check(webview_->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
      [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
        args->put_Cancel(TRUE); return S_OK;
      }).Get(), &token), "add_FrameNavigationStarting");
    check(webview_->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
      [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
        args->put_Handled(TRUE); return S_OK;
      }).Get(), &token), "add_NewWindowRequested");
    check(webview_->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
      [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
        args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY); return S_OK;
      }).Get(), &token), "add_PermissionRequested");
    check(webview_->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
      [weak](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
        if (auto self = weak.lock()) self->fail("WebView2 process failed; reopen the tool");
        return S_OK;
      }).Get(), &token), "add_ProcessFailed");
    check(webview_->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
      [weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
        BOOL success = FALSE; args->get_IsSuccess(&success);
        if (!success) {
          COREWEBVIEW2_WEB_ERROR_STATUS status{}; args->get_WebErrorStatus(&status);
          if (status != COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED)
            if (auto self = weak.lock()) self->fail("WebView2 navigation failed: " + std::to_string(status));
        }
        return S_OK;
      }).Get(), &token), "add_NavigationCompleted");
    check(webview_->AddScriptToExecuteOnDocumentCreated(wide(options_.script).c_str(),
      Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
        [weak](HRESULT result, LPCWSTR) -> HRESULT {
          if (auto self = weak.lock(); self && !self->closed_) {
            if (FAILED(result)) self->fail("Could not inject the JavaScript bridge");
            else {
              auto hr = self->webview_->Navigate(wide(self->uri_).c_str());
              if (FAILED(hr)) self->fail("Could not navigate to the HTML entry point");
              if (self->want_devtools_) self->devtools();
            }
          }
          return S_OK;
        }).Get()), "AddScriptToExecuteOnDocumentCreated");
  }
  void evaluate(const std::string& script) override {
    if (webview_ && !closed_) webview_->ExecuteScript(wide(script).c_str(), nullptr);
  }
  void devtools() override {
    want_devtools_ = true;
    if (webview_) webview_->OpenDevToolsWindow();
  }
  bool closed() const override { return closed_; }
  bool visible() const override {
    auto root = hwnd_ ? GetAncestor(hwnd_, GA_ROOT) : nullptr;
    return hwnd_ && IsWindowVisible(hwnd_) && (!root || !IsIconic(root));
  }
  bool focused() const override {
    auto focus = GetFocus();
    return hwnd_ && (focus == hwnd_ || IsChild(hwnd_, focus));
  }
  void tick() override {
    const bool next = visible();
    if (controller_ && next != visible_) { controller_->put_IsVisible(next); visible_ = next; }
  }
  void focus() override {
    if (closed_ || !hwnd_) return;
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(GetAncestor(hwnd_, GA_ROOT));
    SetFocus(hwnd_);
    if (controller_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  }
  void set_title(const std::string& title) override { SetWindowTextW(hwnd_, wide(title).c_str()); }
  Json diagnostics() const override {
    return {{"backend", "WebView2"}, {"browserVersion", browser_version_}, {"controllerReady", controller_ != nullptr},
      {"controllerVisible", visible_}};
  }
  Json placement() const override {
    if (!hwnd_) return nullptr;
    RECT rect = floating_rect_;
    bool maximized = maximized_;
    if (!(GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_CHILD)) {
      WINDOWPLACEMENT placement{}; placement.length = sizeof(placement);
      if (!GetWindowPlacement(hwnd_, &placement)) return nullptr;
      rect = placement.rcNormalPosition;
      MONITORINFO monitor{sizeof(monitor)};
      if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor))
        OffsetRect(&rect, monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top);
      maximized = placement.showCmd == SW_SHOWMAXIMIZED ||
        (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED));
    }
    return {{"x", rect.left}, {"y", rect.top}, {"width", rect.right - rect.left}, {"height", rect.bottom - rect.top}, {"maximized", maximized}};
  }
  void restore_placement(const Json& value) override {
    RECT rect{value.at("x").get<LONG>(), value.at("y").get<LONG>(), 0, 0};
    const auto width = value.at("width").get<LONG>(), height = value.at("height").get<LONG>();
    rect.right = rect.left + width; rect.bottom = rect.top + height;
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &monitor);
    const auto w = std::min(width, monitor.rcWork.right - monitor.rcWork.left);
    const auto h = std::min(height, monitor.rcWork.bottom - monitor.rcWork.top);
    rect.left = std::clamp(rect.left, monitor.rcWork.left, monitor.rcWork.right - w);
    rect.top = std::clamp(rect.top, monitor.rcWork.top, monitor.rcWork.bottom - h);
    rect.right = rect.left + w; rect.bottom = rect.top + h;
    floating_rect_ = rect;
    maximized_ = value.value("maximized", false);
    if (IsZoomed(hwnd_) || IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetWindowPos(hwnd_, nullptr, rect.left, rect.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ShowWindow(hwnd_, maximized_ ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE);
  }
  void* native_handle() const override { return hwnd_; }
  void prepare_dock() override {
    const auto saved = placement();
    maximized_ = saved.value("maximized", false);
    if (maximized_) ShowWindow(hwnd_, SW_RESTORE);
    GetWindowRect(hwnd_, &floating_rect_);
  }
  void restore_floating() override {
    SetParent(hwnd_, nullptr);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN);
    SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(options_.parent));
    restore_placement({{"x", floating_rect_.left}, {"y", floating_rect_.top},
      {"width", floating_rect_.right - floating_rect_.left}, {"height", floating_rect_.bottom - floating_rect_.top}, {"maximized", maximized_}});
    if (controller_) controller_->NotifyParentWindowPositionChanged();
    focus();
  }
};

struct EnvironmentState {
  ComPtr<ICoreWebView2Environment> environment;
  std::vector<std::weak_ptr<WinWindow>> waiting;
  std::string error;
};
class WinPlatform final : public Platform {
  std::shared_ptr<EnvironmentState> state_ = std::make_shared<EnvironmentState>();
  HINSTANCE instance_ = nullptr;
  bool com_ = false;
  HWND clipboard_owner_ = nullptr;
public:
  explicit WinPlatform(const fs::path& data) {
    check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "CoInitializeEx (WebView2 requires STA)");
    com_ = true;
    // COM completion handlers can outlive extension teardown. Keep their code mapped until process exit.
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(&WinWindow::proc), &instance_);
    WNDCLASSW cls{}; cls.lpfnWndProc = WinWindow::proc; cls.hInstance = instance_;
    cls.lpszClassName = window_class; cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      CoUninitialize(); com_ = false; throw std::runtime_error("RegisterClass failed");
    }
    std::weak_ptr<EnvironmentState> weak = state_;
    auto hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, data.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [weak](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
          auto state = weak.lock();
          if (!state) return S_OK;
          if (FAILED(result) || !environment) state->error = "WebView2 initialization failed. Install Microsoft Edge WebView2 Evergreen Runtime. HRESULT " + std::to_string(static_cast<unsigned long>(result));
          else state->environment = environment;
          auto waiting = std::move(state->waiting);
          for (auto& item : waiting) if (auto window = item.lock()) {
            try {
              if (!state->error.empty()) window->fail(state->error);
              else window->initialize(environment);
            } catch (const std::exception& e) { window->fail(e.what()); }
          }
          return S_OK;
        }).Get());
    if (FAILED(hr)) {
      UnregisterClassW(window_class, instance_); CoUninitialize(); com_ = false;
      throw std::runtime_error("WebView2 Runtime is unavailable. Install Microsoft Edge WebView2 Evergreen Runtime.");
    }
  }
  ~WinPlatform() override {
    if (clipboard_owner_) DestroyWindow(clipboard_owner_);
    state_.reset(); UnregisterClassW(window_class, instance_);
    if (com_) CoUninitialize();
  }
  void desktop(const std::string& method, const Json& args, DesktopReply reply) override {
    if (method == "ReaWeb_OpenExternal") {
      const auto url = args[0].get<std::string>(); validate_external_url(url);
      if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        throw Error("EXTERNAL_OPEN_FAILED", "The system could not open this link");
      reply({{"result", true}}); return;
    }
    if (!clipboard_owner_) clipboard_owner_ = CreateWindowExW(0, L"STATIC", L"ReaWebAPI Clipboard", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance_, nullptr);
    if (!clipboard_owner_ || !OpenClipboard(clipboard_owner_)) throw Error("CLIPBOARD_BUSY", "The clipboard is busy; try again");
    struct Close { ~Close() { CloseClipboard(); } } close;
    if (method == "ReaWeb_ClipboardReadText") {
      auto handle = GetClipboardData(CF_UNICODETEXT);
      if (!handle) { reply({{"result", ""}}); return; }
      const auto count = GlobalSize(handle) / sizeof(wchar_t);
      if (count > value_limit) throw Error("BUFFER_LIMIT", "Clipboard exceeds the text limit");
      auto data = static_cast<const wchar_t*>(GlobalLock(handle));
      if (!data) throw Error("CLIPBOARD_ERROR", "Cannot read clipboard");
      std::string result;
      try { result = utf8(std::wstring(data, std::find(data, data + count, L'\0')).c_str()); }
      catch (...) { GlobalUnlock(handle); throw; }
      GlobalUnlock(handle);
      if (result.size() > value_limit) throw Error("BUFFER_LIMIT", "Clipboard exceeds 16 MiB");
      reply({{"result", result}}); return;
    }
    const auto text = wide(args[0].get<std::string>());
    auto handle = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!handle) throw Error("CLIPBOARD_ERROR", "Cannot allocate clipboard text");
    auto data = GlobalLock(handle);
    if (!data) { GlobalFree(handle); throw Error("CLIPBOARD_ERROR", "Cannot lock clipboard memory"); }
    std::memcpy(data, text.c_str(), (text.size() + 1) * sizeof(wchar_t)); GlobalUnlock(handle);
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, handle)) {
      GlobalFree(handle); throw Error("CLIPBOARD_ERROR", "Cannot write clipboard");
    }
    reply({{"result", true}});
  }
  std::shared_ptr<Window> open(WindowOptions options) override {
    if (!state_->error.empty()) throw std::runtime_error(state_->error);
    auto window = std::make_shared<WinWindow>(std::move(options), instance_);
    if (state_->environment) window->initialize(state_->environment.Get());
    else state_->waiting.push_back(window);
    return window;
  }
};
}
std::unique_ptr<Platform> make_platform(const fs::path& data) { return std::make_unique<WinPlatform>(data); }
}
