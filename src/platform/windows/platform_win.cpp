#include "platform/platform.hpp"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl.h>
#include <WebView2.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include "platform/windows/win_devtools.hpp"
#include "platform/windows/win_icon.hpp"
#include "platform/windows/win_context_menu.hpp"

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
class DragSource final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropSource> {
public:
  std::function<bool()> alive;
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
    if (escape || !alive()) return DRAGDROP_S_CANCEL;
    return keys & MK_LBUTTON ? S_OK : DRAGDROP_S_DROP;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};
class DragText final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject> {
public:
  std::wstring text;
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override {
    if (!f) return E_POINTER;
    return f->cfFormat == CF_UNICODETEXT && (f->tymed & TYMED_HGLOBAL) && f->dwAspect == DVASPECT_CONTENT && f->lindex == -1 ? S_OK : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* medium) override {
    if (!medium) return E_POINTER;
    if (FAILED(QueryGetData(f))) return DV_E_FORMATETC;
    auto memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!memory) return E_OUTOFMEMORY;
    auto bytes = GlobalLock(memory);
    if (!bytes) { GlobalFree(memory); return E_OUTOFMEMORY; }
    std::memcpy(bytes, text.c_str(), (text.size() + 1) * sizeof(wchar_t)); GlobalUnlock(memory);
    *medium = {}; medium->tymed = TYMED_HGLOBAL; medium->hGlobal = memory; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return DATA_E_FORMATETC; }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* f) override { if (f) f->ptd = nullptr; return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** result) override {
    if (direction != DATADIR_GET) return E_NOTIMPL;
    FORMATETC f{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; return SHCreateStdEnumFmtEtc(1, &f, result);
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

class WinWindow final : public Window, public std::enable_shared_from_this<WinWindow> {
  WindowOptions options_;
  std::string uri_;
  HWND hwnd_ = nullptr;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> webview_;
  bool closed_ = false;
  std::unique_ptr<WinDevTools> devtools_;
  static constexpr UINT toggle_devtools_message = DevToolsKeys::toggle_message;
  RECT floating_rect_{};
  bool maximized_ = false;
  std::string browser_version_;
  bool visible_ = true;
  bool drop_enabled_ = false, dragging_ = false;
  WinIcon icon_;
  static constexpr UINT dock_command = 0x1800;
  void layout() {
    RECT rect{}; GetClientRect(hwnd_, &rect);
    if (devtools_) rect = devtools_->layout(rect);
    if (controller_) controller_->put_Bounds(rect);
  }
public:
  static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto self = reinterpret_cast<WinWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
      self = static_cast<WinWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      self->hwnd_ = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) {
      if (msg == WM_SYSCOMMAND && (wp & 0xfff0) == dock_command) {
        if (self->options_.on_dock_toggle) self->options_.on_dock_toggle();
        return 0;
      } else if (msg == WM_INITMENU && reinterpret_cast<HMENU>(wp) == GetSystemMenu(hwnd, FALSE)) {
        CheckMenuItem(reinterpret_cast<HMENU>(wp), dock_command, MF_BYCOMMAND |
          (self->options_.is_docked && self->options_.is_docked() ? MF_CHECKED : MF_UNCHECKED));
      } else if (msg == WM_DPICHANGED) {
        if (!(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD)) {
          const auto& rect = *reinterpret_cast<RECT*>(lp);
          SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        self->layout(); return 0;
      } else if (msg == toggle_devtools_message) {
        if (self->devtools_) self->devtools_->toggle(); return 0;
      } else if (msg == WM_KEYDOWN && wp == 'I' && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
        if (!(lp & (1LL << 30))) PostMessageW(hwnd, toggle_devtools_message, 0, 0);
        return 0;
      } else if (msg == WM_SIZE && self->controller_) {
        self->layout();
      } else if (msg == WM_MOVE && self->controller_) {
        self->controller_->NotifyParentWindowPositionChanged();
      } else if (msg == WM_SETFOCUS && self->controller_) {
        self->controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      } else if (msg == WM_CLOSE) {
        if (self->options_.on_close) self->options_.on_close(); else self->closed_ = true;
        return 0;
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
    if (options_.on_dock_toggle) {
      auto menu = GetSystemMenu(hwnd_, FALSE);
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, dock_command, L"Dock in REAPER");
    }
    try { devtools_ = std::make_unique<WinDevTools>(hwnd_, [this] { layout(); }, [this] { focus(); }); }
    catch (...) { DestroyWindow(hwnd_); throw; }
    // A REAPER-owned floating window stays above its owner when focus returns to REAPER.
    SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(options_.parent));
    ShowWindow(hwnd_, SW_SHOW);
  }
  ~WinWindow() override {
    closed_ = true;
    if (devtools_) devtools_->detach();
    if (controller_) controller_->Close();
    devtools_.reset();
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
    layout();
    ComPtr<ICoreWebView2Settings> settings;
    check(webview_->get_Settings(&settings), "get_Settings");
    settings->put_AreDevToolsEnabled(TRUE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_AreDefaultScriptDialogsEnabled(FALSE);
    ComPtr<ICoreWebView2Settings3> settings3;
    if (SUCCEEDED(settings.As(&settings3))) settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    EventRegistrationToken token{};
    auto weak = weak_from_this();
    check(install_window_menu(webview_.Get(), options_.on_dock_toggle ? std::function<void()>([weak] {
      if (auto self = weak.lock(); self && !self->closed_) self->options_.on_dock_toggle();
    }) : std::function<void()>(), [weak] {
      auto self = weak.lock();
      return self && !self->closed_ && self->options_.is_docked && self->options_.is_docked();
    }, [weak](DevToolsAction action) {
      if (auto self = weak.lock(); self && !self->closed_) self->devtools_->perform(action);
    }, [weak] {
      auto self = weak.lock();
      return self && !self->closed_ ? self->devtools_->menu_state() : DevToolsMenuState{};
    }), "Install window context menu");
    check(controller_->add_AcceleratorKeyPressed(Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
      [weak](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
        UINT key = 0; COREWEBVIEW2_KEY_EVENT_KIND kind{}; COREWEBVIEW2_PHYSICAL_KEY_STATUS status{};
        args->get_VirtualKey(&key); args->get_KeyEventKind(&kind); args->get_PhysicalKeyStatus(&status);
        if (key == 'I' && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
          args->put_Handled(TRUE);
          if (kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && !status.WasKeyDown)
            if (auto self = weak.lock(); self && !self->closed_) PostMessageW(self->hwnd_, toggle_devtools_message, 0, 0);
        }
        return S_OK;
      }).Get(), &token), "add_AcceleratorKeyPressed");
    check(webview_->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
      [weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
        if (auto self = weak.lock(); self && !self->closed_) {
          LPWSTR source = nullptr, message = nullptr;
          args->get_Source(&source);
          if (same_document(utf8(source), self->uri_) && SUCCEEDED(args->TryGetWebMessageAsString(&message))) {
            const auto text = utf8(message);
            if (text.rfind("{\"__reawebNativeDrop\":", 0) == 0) self->native_drop(args, text);
            else self->options_.on_message(text);
          }
          CoTaskMemFree(source); CoTaskMemFree(message);
        }
        return S_OK;
      }).Get(), &token), "add_WebMessageReceived");
    check(webview_->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
      [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
        LPWSTR uri = nullptr; args->get_Uri(&uri);
        auto self = weak.lock();
        if (!self || !same_document(utf8(uri), self->uri_)) args->put_Cancel(TRUE);
        else if (self->options_.on_reload && self->options_.on_reload()) args->put_Cancel(TRUE);
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
            }
          }
          return S_OK;
        }).Get()), "AddScriptToExecuteOnDocumentCreated");
  }
  void evaluate(const std::string& script) override {
    if (webview_ && !closed_) webview_->ExecuteScript(wide(script).c_str(), nullptr);
  }
  void devtools() override {
    if (devtools_) devtools_->open();
  }
  Json devtools_state() const override { return devtools_->state(); }
  void restore_devtools(const Json& value) override { devtools_->restore(value); }
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
    if (!closed_) icon_.refresh(hwnd_);
    if (!closed_ && devtools_) devtools_->tick(webview_.Get());
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
  std::vector<int> icon_sizes() const override {
    const auto dpi = GetDpiForWindow(hwnd_);
    return {std::clamp(GetSystemMetricsForDpi(SM_CXSMICON, dpi), 8, 256), std::clamp(GetSystemMetricsForDpi(SM_CXICON, dpi), 8, 256)};
  }
  void set_icon(const std::vector<IconBitmap>& images) override { icon_.set(hwnd_, images); }
  void clear_icon() override { icon_.clear(hwnd_); }
  void set_icon_visible(bool visible) override { icon_.set_visible(hwnd_, visible); }
  void set_visible(bool visible) override { ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE); }
  void reload() override { if (webview_) webview_->Reload(); }
  Json bounds() const override {
    RECT rect{}; if (!hwnd_ || !GetWindowRect(hwnd_, &rect)) return nullptr;
    return {{"x", rect.left}, {"y", rect.top}, {"width", rect.right - rect.left}, {"height", rect.bottom - rect.top}};
  }
  void set_drop_enabled(bool enabled) override {
    if (enabled) {
      ComPtr<ICoreWebView2_23> modern;
      if (!webview_ || FAILED(webview_.As(&modern))) throw Error("HOST_UNAVAILABLE", "Native drop requires a current WebView2 Runtime");
    }
    drop_enabled_ = enabled;
  }
  void native_drop(ICoreWebView2WebMessageReceivedEventArgs* args, const std::string& message) {
    if (!drop_enabled_ || !options_.on_drop || message.size() > message_limit) return;
    try {
      const auto input = Json::parse(message).at("__reawebNativeDrop");
      const auto text = input.at("text").get<std::string>();
      if (text.size() > value_limit || text.find('\0') != std::string::npos) throw Error("BUFFER_LIMIT", "Invalid drop text");
      ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> extended;
      check(args->QueryInterface(IID_PPV_ARGS(&extended)), "Native dropped objects");
      ComPtr<ICoreWebView2ObjectCollectionView> objects; check(extended->get_AdditionalObjects(&objects), "Dropped objects");
      UINT32 count = 0; if (objects) check(objects->get_Count(&count), "Dropped file count");
      if (count > 256) throw Error("BUFFER_LIMIT", "At most 256 files may be dropped");
      Json files = Json::array();
      for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IUnknown> item; check(objects->GetValueAtIndex(i, &item), "Dropped file");
        ComPtr<ICoreWebView2File> file; check(item.As(&file), "Dropped native file");
        LPWSTR path = nullptr; check(file->get_Path(&path), "Dropped file path");
        auto value = utf8(path); CoTaskMemFree(path);
        if (value.empty() || value.size() > 32768) throw Error("INVALID_PATH", "Dropped File has no native path");
        files.push_back(value);
      }
      if (files.empty() && text.empty()) return;
      options_.on_drop({{"document", input.at("document")}, {"files", files}, {"text", text},
        {"x", input.at("x")}, {"y", input.at("y")}});
    } catch (const std::exception& error) {
      evaluate("console.error('[ReaWebAPI native drop]'," + Json(error.what()).dump() + ");");
    }
  }
  void start_drag(const Json& payload, Reply reply) override {
    if (dragging_) throw Error("DRAG_BUSY", "A native drag is already active");
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) throw Error("DRAG_GESTURE_REQUIRED", "Start a native drag while the left mouse button is held");
    ComPtr<IDataObject> data;
    if (!payload.at("files").empty()) {
      std::vector<PIDLIST_ABSOLUTE> ids;
      struct Release { std::vector<PIDLIST_ABSOLUTE>& ids; ~Release() { for (auto id : ids) CoTaskMemFree(id); } } release{ids};
      for (const auto& path : payload.at("files")) {
        auto id = ILCreateFromPathW(wide(path.get<std::string>()).c_str());
        if (!id) throw Error("INVALID_PATH", "Cannot prepare file for native drag");
        ids.push_back(id);
      }
      ComPtr<IShellItemArray> items;
      check(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), const_cast<PCIDLIST_ABSOLUTE*>(ids.data()), &items), "Native drag files");
      check(items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)), "Native file data object");
    } else {
      auto object = Microsoft::WRL::Make<DragText>(); object->text = wide(payload.at("text").get<std::string>()); data = object;
    }
    auto source = Microsoft::WRL::Make<DragSource>();
    source->alive = [weak = weak_from_this()] { auto window = weak.lock(); return window && !window->closed(); };
    dragging_ = true; DWORD effect = DROPEFFECT_NONE;
    const auto result = DoDragDrop(data.Get(), source.Get(), DROPEFFECT_COPY, &effect); dragging_ = false;
    if (FAILED(result)) throw Error("DRAG_FAILED", "The system could not start the drag");
    reply({{"result", result == DRAGDROP_S_DROP && (effect & DROPEFFECT_COPY) != 0}});
  }
  Json diagnostics() const override {
    return {{"backend", "WebView2"}, {"browserVersion", browser_version_}, {"controllerReady", controller_ != nullptr},
      {"controllerVisible", visible_}, {"devtools", devtools_->diagnostics()}};
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
    icon_.refresh(hwnd_);
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
    check(OleInitialize(nullptr), "OleInitialize (WebView2 and native drag require STA)");
    com_ = true;
    // COM completion handlers can outlive extension teardown. Keep their code mapped until process exit.
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(&WinWindow::proc), &instance_);
    WNDCLASSW cls{}; cls.lpfnWndProc = WinWindow::proc; cls.hInstance = instance_;
    cls.lpszClassName = window_class; cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      OleUninitialize(); com_ = false; throw std::runtime_error("RegisterClass failed");
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
      UnregisterClassW(window_class, instance_); OleUninitialize(); com_ = false;
      throw std::runtime_error("WebView2 Runtime is unavailable. Install Microsoft Edge WebView2 Evergreen Runtime.");
    }
  }
  ~WinPlatform() override {
    if (clipboard_owner_) DestroyWindow(clipboard_owner_);
    state_.reset(); UnregisterClassW(window_class, instance_);
    if (com_) OleUninitialize();
  }
  void desktop(const std::string& method, const Json& args, DesktopReply reply) override {
    if (method == "ReaWeb_RevealPath") {
      auto id = ILCreateFromPathW(wide(args.at(0).get<std::string>()).c_str());
      if (!id) throw Error("INVALID_PATH", "Cannot locate file in Explorer");
      const auto result = SHOpenFolderAndSelectItems(id, 0, nullptr, 0); CoTaskMemFree(id);
      if (FAILED(result)) throw Error("REVEAL_FAILED", "Explorer could not reveal the path");
      reply({{"result", true}}); return;
    }
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
