#include "platform.hpp"
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <vector>

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
      } else if (msg == WM_SETFOCUS && self->controller_) {
        self->controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      } else if (msg == WM_CLOSE) {
        DestroyWindow(hwnd); return 0;
      } else if (msg == WM_NCDESTROY) {
        self->closed_ = true; self->hwnd_ = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  WinWindow(WindowOptions options, HINSTANCE instance) : options_(std::move(options)), uri_(file_uri(options_.entry)) {
    hwnd_ = CreateWindowExW(0, window_class, wide(options_.title).c_str(), WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, 860, 640, nullptr, nullptr, instance, this);
    if (!hwnd_) throw std::runtime_error("CreateWindowEx failed");
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
    state_.reset(); UnregisterClassW(window_class, instance_);
    if (com_) CoUninitialize();
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
