#include "platform/platform.hpp"
#include "platform/shared/navigation.hpp"
#include "platform/shared/page_background.hpp"
#include "platform/shared/transparency.hpp"
#include "platform/shared/startup.hpp"
#include "platform/shared/idle_pool.hpp"
#include "platform/shared/drag_regions.hpp"
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include "platform/shared/color_profile.hpp"
#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath>
#include "platform/windows/win_devtools.hpp"
#include "platform/windows/win_icon.hpp"
#include "platform/windows/win_dock_redraw.hpp"
#include "platform/windows/win_cursor.hpp"
#include "platform/windows/win_resize_edges.hpp"
#include "platform/windows/win_context_menu.hpp"
#include "platform/windows/win_resources.hpp"
#include "platform/windows/win_overlay.hpp"
#include "platform/windows/win_video_presenter.hpp"
#include "platform/shared/window_menu.hpp"

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
void open_external(const std::string& url) {
  validate_external_url(url);
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
    throw Error("EXTERNAL_OPEN_FAILED", "The system could not open this link");
}
constexpr wchar_t window_class[] = L"ReaWebAPI.Window";
class WinIdleView final : public std::enable_shared_from_this<WinIdleView> {
  HWND parent_ = nullptr;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> view_;
  EventRegistrationToken loaded_{}, process_{};
public:
  bool ready = false, failed = false;
  explicit WinIdleView(HINSTANCE instance) {
    parent_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP,
      0, 0, 860, 640, nullptr, nullptr, instance, nullptr);
    if (!parent_) throw std::runtime_error("Cannot create an idle WebView host");
  }
  ~WinIdleView() {
    if (controller_) controller_->Close();
    view_.Reset(); controller_.Reset();
    if (parent_) DestroyWindow(parent_);
  }
  void start(ICoreWebView2Environment* environment) {
    auto weak = weak_from_this();
    check(environment->CreateCoreWebView2Controller(parent_,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([weak](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
        auto self = weak.lock();
        if (!self) { if (controller) controller->Close(); return S_OK; }
        try {
          check(result, "Prepare idle WebView");
          if (!controller) throw std::runtime_error("Idle WebView controller unavailable");
          self->controller_ = controller;
          check(controller->put_IsVisible(FALSE), "Hide idle WebView");
          check(controller->get_CoreWebView2(&self->view_), "Idle WebView");
          check(self->view_->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
              if (auto slot = weak.lock()) {
                BOOL success = FALSE; args->get_IsSuccess(&success);
                slot->ready = success != FALSE; slot->failed = !success;
              }
              return S_OK;
            }).Get(), &self->loaded_), "Idle WebView navigation");
          check(self->view_->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
              if (auto slot = weak.lock()) slot->failed = true;
              return S_OK;
            }).Get(), &self->process_), "Idle WebView process");
          check(self->view_->Navigate(L"about:blank"), "Load idle WebView");
        } catch (const std::exception&) { self->failed = true; }
        return S_OK;
      }).Get()), "Prepare idle WebView controller");
  }
  ComPtr<ICoreWebView2Controller> attach(HWND parent) {
    if (!ready || failed || FAILED(controller_->put_ParentWindow(parent))) return {};
    view_->remove_NavigationCompleted(loaded_); view_->remove_ProcessFailed(process_);
    view_.Reset();
    return std::move(controller_);
  }
};
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
  std::unique_ptr<WinOverlay> overlay_;
  std::string uri_;
  HWND hwnd_ = nullptr;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> webview_;
  bool closed_ = false;
  std::unique_ptr<WinDevTools> devtools_;
  static constexpr UINT toggle_devtools_message = DevToolsKeys::toggle_message;
  static constexpr UINT focus_page_message = WM_APP + 74;
  HWND fullscreen_host_ = nullptr, fullscreen_parent_ = nullptr;
  RECT fullscreen_restore_{};
  LONG_PTR fullscreen_style_ = 0;
  void element_fullscreen(bool enter) {
    if (enter == bool(fullscreen_host_) || !hwnd_) return;
    if (enter) {
      fullscreen_style_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
      fullscreen_parent_ = (fullscreen_style_ & WS_CHILD) ? GetParent(hwnd_) : GetWindow(hwnd_, GW_OWNER);
      GetWindowRect(hwnd_, &fullscreen_restore_);
      MONITORINFO monitor{sizeof(monitor)};
      if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        webview_->ExecuteScript(L"document.exitFullscreen().catch(()=>{})", nullptr); return;
      }
      const auto& bounds = monitor.rcMonitor;
      fullscreen_host_ = CreateWindowExW(WS_EX_APPWINDOW, L"STATIC", L"Video fullscreen", WS_POPUP | WS_CLIPCHILDREN,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      if (!fullscreen_host_) { webview_->ExecuteScript(L"document.exitFullscreen().catch(()=>{})", nullptr); return; }
      SetWindowSubclass(fullscreen_host_, [](HWND host, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) -> LRESULT {
        auto* self = reinterpret_cast<WinWindow*>(data);
        if (message == WM_CLOSE || (message == WM_KEYDOWN && wp == VK_ESCAPE)) {
          if (self->webview_) self->webview_->ExecuteScript(L"document.exitFullscreen().catch(()=>{})", nullptr);
          return 0;
        }
        if (message == WM_SIZE && self->hwnd_) {
          RECT rect{}; GetClientRect(host, &rect);
          SetWindowPos(self->hwnd_, nullptr, 0, 0, rect.right, rect.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return DefSubclassProc(host, message, wp, lp);
      }, 1, reinterpret_cast<DWORD_PTR>(this));
      SetWindowLongPtrW(hwnd_, GWL_STYLE, (fullscreen_style_ & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME)) | WS_CHILD);
      SetParent(hwnd_, fullscreen_host_);
      SetWindowPos(hwnd_, HWND_TOP, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
      ShowWindow(fullscreen_host_, SW_SHOW); SetForegroundWindow(fullscreen_host_);
    } else {
      const auto host = fullscreen_host_; fullscreen_host_ = nullptr;
      SetParent(hwnd_, (fullscreen_style_ & WS_CHILD) ? fullscreen_parent_ : nullptr);
      SetWindowLongPtrW(hwnd_, GWL_STYLE, fullscreen_style_);
      if (!(fullscreen_style_ & WS_CHILD)) SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(fullscreen_parent_));
      auto rect = fullscreen_restore_;
      if ((fullscreen_style_ & WS_CHILD) && IsWindow(fullscreen_parent_)) {
        GetClientRect(fullscreen_parent_, &rect);
      }
      SetWindowPos(hwnd_, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
      DestroyWindow(host);
    }
    layout();
    if (!closed_ && controller_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  }
  HWND hosted_parking_ = nullptr;
  RECT floating_rect_{};
  bool maximized_ = false;
  std::string browser_version_;
  Json environment_timing_;
  bool controller_prewarmed_ = false;
  void mark_startup(StartupTiming::Stage stage) { if (options_.startup_timing) options_.startup_timing->mark(stage); }
  bool visible_ = true;
  bool drop_enabled_ = false, dragging_ = false;
  WinIcon icon_;
  WinDockRedraw dock_redraw_;
  WinCursor cursor_;
  WinResizeEdges resize_edges_;
  COLORREF background_ = RGB(255, 255, 255);
  StartupPresentation startup_;
  HWND startup_foreground_ = GetForegroundWindow();
  bool snapshot_pending_ = false;
  unsigned snapshot_requested_ = ~0u;
  bool startup_region_ = false;
  bool drag_regions_supported_ = false;
  bool alpha_ready_ = false, alpha_supported_ = false;
  std::shared_ptr<WinVideoHost> video_host_;
  std::weak_ptr<WinVideoPresenter> video_presenter_;
  bool video_background_active_ = false;
  bool update_video_clipping() noexcept {
    if (!hwnd_ || !IsWindow(hwnd_)) return true;
    const auto style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    const auto next = video_background_active_ ? style & ~LONG_PTR(WS_CLIPCHILDREN) : style | WS_CLIPCHILDREN;
    if (next == style) return true;
    SetLastError(0);
    if (!SetWindowLongPtrW(hwnd_, GWL_STYLE, next) && GetLastError()) return false;
    const bool changed = SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) != FALSE;
    InvalidateRect(hwnd_, nullptr, TRUE);
    return changed;
  }
  void clear_video_surface() {
    if (auto presenter = video_presenter_.lock()) { presenter->invalidate(); presenter->detach_view(); }
    video_presenter_.reset();
    if (video_background_active_) {
      ComPtr<ICoreWebView2Controller2> alpha;
      if (controller_ && SUCCEEDED(controller_.As(&alpha)))
        alpha->put_DefaultBackgroundColor(options_.transparent ? COREWEBVIEW2_COLOR{0,0,0,0} :
          COREWEBVIEW2_COLOR{255,GetRValue(background_),GetGValue(background_),GetBValue(background_)});
      video_background_active_ = false;
      update_video_clipping();
    }
  }
  void update_video_surface() {
    if (!video_background_active_) return;
    const auto presenter = video_presenter_.lock();
    if (!presenter || presenter->closed()) { clear_video_surface(); return; }
    RECT bounds{}; controller_->get_Bounds(&bounds);
    double zoom = 1, scale = GetDpiForWindow(hwnd_) / 96.0;
    controller_->get_ZoomFactor(&zoom);
    ComPtr<ICoreWebView2Controller3> raster;
    if (SUCCEEDED(controller_.As(&raster))) raster->get_RasterizationScale(&scale);
    try { presenter->update_view(bounds, zoom * scale, visible() && !IsIconic(GetAncestor(hwnd_, GA_ROOT))); }
    catch (...) { clear_video_surface(); }
  }
  double corner_radius_ = 0;
  bool custom_region_ = false;
  int region_width_ = -1, region_height_ = -1, region_radius_ = -1;
  static bool desktop_composition() {
    BOOL enabled = FALSE;
    return SUCCEEDED(DwmIsCompositionEnabled(&enabled)) && enabled;
  }
  void configure_transparency() {
    if (!options_.transparent) return;
    if (!desktop_composition()) throw Error("TRANSPARENCY_UNAVAILABLE", "Desktop Alpha requires DWM composition");
    // Keep the HWND controller and its native input/drag-and-drop routing.
    const auto region = CreateRectRgn(-1, -1, 0, 0);
    if (!region) throw Error("TRANSPARENCY_FAILED", "Cannot allocate the DWM Alpha region");
    DWM_BLURBEHIND blur{};
    blur.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    blur.fEnable = TRUE; blur.hRgnBlur = region;
    const auto result = DwmEnableBlurBehindWindow(hwnd_, &blur);
    DeleteObject(region);
    if (FAILED(result)) throw Error("TRANSPARENCY_FAILED", "Cannot enable DWM desktop Alpha: " + std::to_string(result));
  }
  bool floating() const { return hwnd_ && !(GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_CHILD); }
  DWORD floating_style() const {
    return (WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN) & (options_.resizable ? ~DWORD{0} : ~DWORD(WS_THICKFRAME | WS_MAXIMIZEBOX));
  }
  int resize_border() const { return options_.resizable && !IsZoomed(hwnd_) ? std::max(4, MulDiv(6, GetDpiForWindow(hwnd_), 96)) : 0; }
  void configure_frame() {
    if (!floating()) return;
    const auto policy = options_.frameless && !options_.transparent ? DWMNCRP_DISABLED : DWMNCRP_ENABLED;
    check(DwmSetWindowAttribute(hwnd_, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy)), "Configure frameless border");
    const DWORD corners = options_.frameless ? 1 : 0; // DWMWCP_DONOTROUND / DEFAULT, Windows 11.
    DwmSetWindowAttribute(hwnd_, 33, &corners, sizeof(corners));
  }
  void update_region() {
    if (!hwnd_ || startup_region_) return;
    if (!custom_region_) return;
    RECT rect{}; GetWindowRect(hwnd_, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    const int radius = corner_radius_active() ? std::min({int(std::lround(corner_radius_ * GetDpiForWindow(hwnd_) / 96.0)), width / 2, height / 2}) : 0;
    if (width == region_width_ && height == region_height_ && radius == region_radius_) return;
    HRGN region = radius ? CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2) : nullptr;
    if (!radius && frameless_active() && !IsZoomed(hwnd_)) region = CreateRectRgn(0, 0, width, height);
    if (radius && !region) return;
    if (SetWindowRgn(hwnd_, region, TRUE)) { region_width_ = width; region_height_ = height; region_radius_ = radius; }
    else if (region) DeleteObject(region);
  }
  void prepare_startup() {
    snapshot_requested_ = startup_.generation();
    if (!startup_.pending() || snapshot_pending_ || closed_) return;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) { fail("Cannot allocate the initial WebView2 capture"); return; }
    snapshot_pending_ = true;
    mark_startup(StartupTiming::FrameRequested);
    const auto generation = startup_.generation();
    // Hosted captures confirm readiness only. Their encoded pixels are never displayed.
    const auto format = options_.hosted ? COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_JPEG : COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG;
    const auto hr = webview_->CapturePreview(format, stream.Get(),
      Callback<ICoreWebView2CapturePreviewCompletedHandler>([weak = weak_from_this(), stream, generation](HRESULT result) -> HRESULT {
        if (auto self = weak.lock(); self && !self->closed_) {
          self->snapshot_pending_ = false;
          if (generation != self->startup_.generation()) {
            if (self->snapshot_requested_ == self->startup_.generation()) self->prepare_startup();
          } else if (SUCCEEDED(result)) { self->mark_startup(StartupTiming::FrameReady); self->startup_.prepared(generation); self->tick(); }
          else self->fail("WebView2 could not prepare the initial frame");
        }
        return S_OK;
      }).Get());
    if (FAILED(hr)) { snapshot_pending_ = false; fail("WebView2 could not capture the initial frame"); }
  }
  void set_background(unsigned color) {
    if (options_.transparent) return;
    const auto next = RGB((color >> 16) & 255, (color >> 8) & 255, color & 255);
    if (background_ == next) return;
    background_ = next;
    ComPtr<ICoreWebView2Controller2> controller;
    if (!video_background_active_ && controller_ && SUCCEEDED(controller_.As(&controller)))
      controller->put_DefaultBackgroundColor({255, GetRValue(next), GetGValue(next), GetBValue(next)});
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (options_.frameless && floating()) RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
  }
  void paint_background(HDC dc) {
    const auto saved = SaveDC(dc);
    // Docker style changes and WM_PRINTCLIENT may supply an unclipped DC.
    for (auto child = video_background_active_ ? nullptr : GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
      if (!IsWindowVisible(child)) continue;
      RECT rect{}; GetWindowRect(child, &rect);
      MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&rect), 2);
      ExcludeClipRect(dc, rect.left, rect.top, rect.right, rect.bottom);
    }
    RECT rect{}; GetClientRect(hwnd_, &rect);
    const auto brush = CreateSolidBrush(video_background_active_ ? RGB(0, 0, 0) : background_);
    FillRect(dc, &rect, brush); DeleteObject(brush);
    if (saved) RestoreDC(dc, saved);
  }
  static constexpr UINT dock_command = 0x1800;
  std::wstring dock_label() const {
    const auto name = options_.app_name ? options_.app_name() : options_.title;
    std::string escaped;
    for (char c : name) {
      if (c == '&') escaped += '&';
      escaped += c == '\t' || c == '\r' || c == '\n' ? ' ' : c;
    }
    return wide("Dock " + escaped + " in Docker");
  }
  void layout() {
    RECT rect{}; GetClientRect(hwnd_, &rect);
    if (devtools_) rect = devtools_->layout(rect);
    if (controller_) controller_->put_Bounds(rect);
    update_video_surface();
    resize_edges_.sync(hwnd_, controller_ && frameless_active() && resize_border());
    update_region();
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
      if (self->overlay_) {
        if (msg == WM_WINDOWPOSCHANGING) self->overlay_->position(reinterpret_cast<WINDOWPOS*>(lp));
        if (msg == WinOverlay::apply_message || msg == WM_MOVE || msg == WM_SIZE || msg == WM_DPICHANGED || msg == WM_CAPTURECHANGED) {
          try { self->overlay_->synchronize(); } catch (const std::exception& e) { self->fail(e.what()); }
        }
        if (msg == WM_WINDOWPOSCHANGED) self->overlay_->repair();
      }
      if (msg == WM_DWMCOMPOSITIONCHANGED && self->options_.transparent) {
        try { self->configure_transparency(); }
        catch (const std::exception& error) { self->fail(error.what()); }
        return 0;
      }
      if (self->options_.frameless && self->floating()) {
        if (msg == WM_NCCALCSIZE) {
          if (wp && IsZoomed(hwnd)) {
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
              reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0] = monitor.rcWork;
          }
          return 0;
        }
        if (msg == WM_NCPAINT) return 0;
        if (msg == WM_NCACTIVATE) return TRUE;
        if (msg == WM_NCHITTEST) {
          if (!self->options_.resizable || IsZoomed(hwnd)) return HTCLIENT;
          RECT r{}; GetWindowRect(hwnd, &r);
          const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
          const int edge = std::max(4, MulDiv(6, GetDpiForWindow(hwnd), 96));
          const bool left = x < r.left + edge, right = x >= r.right - edge;
          const bool top = y < r.top + edge, bottom = y >= r.bottom - edge;
          return top ? (left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP) :
            bottom ? (left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM) : left ? HTLEFT : right ? HTRIGHT : HTCLIENT;
        }
      }
      if ((self->options_.frameless || !self->options_.resizable) && msg == WM_SYSCOMMAND &&
          ((wp & 0xfff0) == SC_SIZE || (wp & 0xfff0) == SC_MAXIMIZE || (wp & 0xfff0) == SC_MOVE)) {
        if (!self->floating() || (!self->options_.resizable && (wp & 0xfff0) != SC_MOVE)) return 0;
      }
      if (self->options_.frameless && !self->floating() &&
          (msg == WM_NCLBUTTONDOWN || msg == WM_NCLBUTTONDBLCLK)) return 0;
      if (!self->options_.hosted && (msg == WM_WINDOWPOSCHANGED || (msg == WM_STYLECHANGED && wp == static_cast<WPARAM>(GWL_STYLE))))
        self->dock_redraw_.sync(hwnd);
      if (msg == WM_STYLECHANGING && wp == static_cast<WPARAM>(GWL_STYLE)) {
        auto& style = reinterpret_cast<STYLESTRUCT*>(lp)->styleNew;
        if (self->video_background_active_) style &= ~DWORD(WS_CLIPCHILDREN);
        else style |= WS_CLIPCHILDREN;
      } else if (msg == WM_ERASEBKGND) {
        self->paint_background(reinterpret_cast<HDC>(wp)); return 1;
      } else if (msg == WM_PAINT) {
        PAINTSTRUCT paint{};
        auto dc = BeginPaint(hwnd, &paint);
        self->paint_background(dc);
        EndPaint(hwnd, &paint); return 0;
      }
      if (msg == WM_CONTEXTMENU && self->devtools_) {
        auto keep_alive = self->shared_from_this();
        try {
          show_window_menu(hwnd, lp, self->options_, self->devtools_->menu_state(),
            [self](DevToolsAction action) { self->devtools_->perform(action); }, [self] { self->reload(); });
        } catch (const std::exception& error) { self->options_.on_error(error.what()); }
        return 0;
      }
      if (msg == WM_SHOWWINDOW) {
        if (wp && !self->options_.hosted) self->icon_.refresh(hwnd, self->icon_host(true));
        else self->cursor_.finish();
      }
      if (msg == WM_SYSCOMMAND && (wp & 0xfff0) == SC_KEYMENU && self->options_.routes_shortcuts && self->options_.routes_shortcuts()) return 0;
      if (msg == WM_SYSCOMMAND && (wp & 0xfff0) == dock_command) {
        if (self->options_.on_dock_toggle) self->options_.on_dock_toggle();
        return 0;
      } else if (msg == WM_INITMENU && reinterpret_cast<HMENU>(wp) == GetSystemMenu(hwnd, FALSE)) {
        ModifyMenuW(reinterpret_cast<HMENU>(wp), dock_command, MF_BYCOMMAND | MF_STRING, dock_command, self->dock_label().c_str());
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
        // DevTools shares the host's input queue across processes. Finish the
        // focus notification before calling back into WebView2.
        PostMessageW(hwnd, focus_page_message, 0, 0);
      } else if (msg == focus_page_message) {
        if (!self->closed_ && self->controller_ && GetFocus() == hwnd &&
            GetForegroundWindow() == GetAncestor(hwnd, GA_ROOT))
          self->controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
      } else if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL && !HIWORD(wp) && !lp)) {
        if (self->options_.on_close) self->options_.on_close(); else self->closed_ = true;
        return 0;
      } else if (msg == WM_NCDESTROY) {
        self->dock_redraw_.detach();
        self->closed_ = true; self->hwnd_ = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  WinWindow(WindowOptions options, HINSTANCE instance) : options_(std::move(options)), uri_(options_.url.empty() ? file_uri(options_.entry) : options_.url), startup_(options_.hidden) {
    if (options_.hosted && options_.overlay.floating_only()) throw Error("WINDOW_HOSTED", "Overlay policies require a standalone App");
    if (options_.transparent) {
      if (!desktop_composition()) throw Error("TRANSPARENCY_UNAVAILABLE", "Desktop Alpha requires DWM composition");
      background_ = RGB(0, 0, 0);
    }
    // REAPER's Docker must be able to find focus inside the embedded inspector.
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT | (options_.frameless || options_.hosted ? 0 : WS_EX_DLGMODALFRAME), window_class, wide(options_.title).c_str(),
      options_.hosted ? WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN : floating_style(),
      options_.hosted ? 0 : CW_USEDEFAULT, options_.hosted ? 0 : CW_USEDEFAULT, 860, 640, static_cast<HWND>(options_.parent), nullptr, instance, this);
    if (!hwnd_) throw std::runtime_error("CreateWindowEx failed");
    cursor_.attach(hwnd_);
    if (options_.on_dock_toggle) {
      auto menu = GetSystemMenu(hwnd_, FALSE);
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, dock_command, dock_label().c_str());
    }
    try { devtools_ = std::make_unique<WinDevTools>(hwnd_, [this] { layout(); }, [this] { focus(); }); }
    catch (...) { DestroyWindow(hwnd_); throw; }
    if (options_.hosted) {
      RECT rect{}; GetClientRect(static_cast<HWND>(options_.parent), &rect);
      SetWindowPos(hwnd_, HWND_TOP, 0, 0, rect.right, rect.bottom, SWP_NOACTIVATE);
      auto region = CreateRectRgn(0, 0, 0, 0);
      if (!region || !SetWindowRgn(hwnd_, region, FALSE)) {
        if (region) DeleteObject(region);
        DestroyWindow(hwnd_); throw std::runtime_error("Cannot prepare the hosted WebView");
      }
      startup_region_ = true;
      ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
      return;
    }
    // A REAPER-owned floating window stays above its owner when focus returns to REAPER.
    SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(options_.parent));
    const BOOL cloaked = TRUE;
    // A cloaked window keeps composing without exposing its empty native surface.
    const auto cloak_result = DwmSetWindowAttribute(hwnd_, DWMWA_CLOAK, &cloaked, sizeof(cloaked));
    if (FAILED(cloak_result)) { DestroyWindow(hwnd_); check(cloak_result, "Cloak initial window"); }
    try {
      configure_frame(); configure_transparency();
      overlay_ = std::make_unique<WinOverlay>();
      overlay_->attach(hwnd_, static_cast<HWND>(options_.parent), options_.overlay, [this] {
        double zoom = 1; if (controller_) controller_->get_ZoomFactor(&zoom);
        return zoom * GetDpiForWindow(hwnd_) / 96.0;
      }, [this](std::string error) { fail(std::move(error)); });
    } catch (...) { overlay_.reset(); DestroyWindow(hwnd_); throw; }
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (!options_.hidden) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  }
  ~WinWindow() override {
    closed_ = true;
    clear_video_surface();
    video_host_.reset();
    element_fullscreen(false);
    overlay_.reset();
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    cursor_.finish();
    dock_redraw_.detach();
    if (devtools_) devtools_->detach();
    if (controller_) controller_->Close();
    devtools_.reset();
    webview_.Reset(); controller_.Reset();
    if (hwnd_) DestroyWindow(hwnd_);
    if (hosted_parking_) DestroyWindow(hosted_parking_);
  }
  void fail(const std::string& error) { clear_video_surface(); closed_ = true; options_.on_error(error); }
  bool environment_wait_cancelled() const { return closed_ || startup_.cancelled(); }
  void initialize(ICoreWebView2Environment* environment, Json timing = nullptr, ComPtr<ICoreWebView2Controller> prepared = {}) {
    if (environment_wait_cancelled()) return;
    environment_timing_ = std::move(timing);
    mark_startup(StartupTiming::EnvironmentReady);
    mark_startup(StartupTiming::ControllerStart);
    if (options_.hosted && (!hwnd_ || !IsWindow(hwnd_) || !IsWindow(static_cast<HWND>(options_.parent)))) {
      closed_ = true; return;
    }
    LPWSTR version = nullptr;
    if (SUCCEEDED(environment->get_BrowserVersionString(&version))) browser_version_ = utf8(version);
    CoTaskMemFree(version);
    if (prepared) {
      controller_ = std::move(prepared); controller_prewarmed_ = true;
      mark_startup(StartupTiming::ControllerReady);
      check(controller_->get_CoreWebView2(&webview_), "get_CoreWebView2");
      check(controller_->put_IsVisible(TRUE), "Activate prepared WebView");
      configure();
      if (visible() && GetFocus() == hwnd_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      return;
    }
    auto weak = weak_from_this();
    const auto hr = environment->CreateCoreWebView2Controller(hwnd_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
      [weak](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
        auto self = weak.lock();
        if (!self || self->closed_) { if (controller) controller->Close(); return S_OK; }
        if (self->options_.hosted && (!self->hwnd_ || !IsWindow(self->hwnd_) ||
            !IsWindow(static_cast<HWND>(self->options_.parent)))) {
          if (controller) controller->Close();
          self->closed_ = true; return S_OK;
        }
        try {
          check(result, "CreateCoreWebView2Controller");
          if (!controller) throw std::runtime_error("WebView2 controller is unavailable");
          self->controller_ = controller;
          self->mark_startup(StartupTiming::ControllerReady);
          check(controller->get_CoreWebView2(&self->webview_), "get_CoreWebView2");
          self->configure();
          if (self->visible() && GetFocus() == self->hwnd_)
            self->controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        } catch (const std::exception& e) { self->fail(e.what()); }
        return S_OK;
      }).Get());
    check(hr, "CreateCoreWebView2Controller");
  }
  void configure() {
    layout();
    ComPtr<ICoreWebView2Controller2> alpha_controller;
    alpha_supported_ = SUCCEEDED(controller_.As(&alpha_controller));
    if (options_.transparent) {
      if (!alpha_supported_) throw Error("TRANSPARENCY_UNAVAILABLE", "Desktop Alpha requires WebView2 Controller2");
      if (FAILED(alpha_controller->put_DefaultBackgroundColor({0, 0, 0, 0})))
        throw Error("TRANSPARENCY_FAILED", "WebView2 rejected the transparent background");
      COREWEBVIEW2_COLOR color{};
      if (FAILED(alpha_controller->get_DefaultBackgroundColor(&color)) || color.A != 0)
        throw Error("TRANSPARENCY_FAILED", "WebView2 did not establish desktop Alpha");
      alpha_ready_ = true;
    }
    ComPtr<ICoreWebView2Settings> settings;
    check(webview_->get_Settings(&settings), "get_Settings");
    settings->put_AreDevToolsEnabled(TRUE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_AreDefaultScriptDialogsEnabled(FALSE);
    ComPtr<ICoreWebView2Settings3> settings3;
    if (SUCCEEDED(settings.As(&settings3))) settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    ComPtr<ICoreWebView2Settings9> settings9;
    drag_regions_supported_ = SUCCEEDED(settings.As(&settings9));
    if (options_.frameless && !drag_regions_supported_)
      throw Error("HOST_UNAVAILABLE", "Frameless windows require WebView2 non-client region support");
    if (drag_regions_supported_) {
      check(settings9->put_IsNonClientRegionSupportEnabled(TRUE), "Enable native drag regions");
    }
    EventRegistrationToken token{};
    auto weak = weak_from_this();
    if (options_.resources) {
      ComPtr<ICoreWebView2_22> resource_view;
      check(webview_.As(&resource_view), "WebView2 resource source filtering");
      check(resource_view->AddWebResourceRequestedFilterWithRequestSourceKinds(L"reaweb://*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,
        COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL), "WebView2 App resource filter");
      check(webview_->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
        [weak](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
          auto self = weak.lock();
          if (!self || self->closed_) return E_ABORT;
          try {
            ComPtr<ICoreWebView2WebResourceRequest> request;
            check(args->get_Request(&request), "App resource request");
            LPWSTR raw_uri = nullptr, raw_method = nullptr;
            request->get_Uri(&raw_uri); request->get_Method(&raw_method);
            const auto uri = utf8(raw_uri), method = utf8(raw_method);
            CoTaskMemFree(raw_uri); CoTaskMemFree(raw_method);
            ComPtr<ICoreWebView2HttpRequestHeaders> native_headers;
            check(request->get_Headers(&native_headers), "App request headers");
            std::map<std::string, std::string> headers;
            for (const auto* name : {"origin", "sec-fetch-site", "range", "if-range", "if-none-match", "if-modified-since"}) {
              LPWSTR value = nullptr;
              if (SUCCEEDED(native_headers->GetHeader(wide(name).c_str(), &value))) headers[name] = utf8(value);
              CoTaskMemFree(value);
            }
            auto response = self->options_.resources->request(uri, method, headers);
            std::string fields;
            for (const auto& [name, value] : response.headers) fields += name + ": " + value + "\r\n";
            ComPtr<ICoreWebView2_2> view2; check(sender->QueryInterface(IID_PPV_ARGS(&view2)), "WebView2 environment");
            ComPtr<ICoreWebView2Environment> environment; check(view2->get_Environment(&environment), "WebView2 environment");
            auto stream = Microsoft::WRL::Make<ResourceStream>(response);
            ComPtr<ICoreWebView2WebResourceResponse> native_response;
            check(environment->CreateWebResourceResponse(stream.Get(), response.status, wide(response.reason()).c_str(), wide(fields).c_str(), &native_response), "App resource response");
            return args->put_Response(native_response.Get());
          } catch (const std::exception& error) { self->fail(error.what()); return E_FAIL; }
        }).Get(), &token), "WebView2 App resources");
    }
    EventRegistrationToken fullscreen_token{};
    check(webview_->add_ContainsFullScreenElementChanged(Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
      [weak](ICoreWebView2* sender, IUnknown*) -> HRESULT {
        if (auto self = weak.lock(); self && !self->closed_) {
          BOOL full = FALSE; sender->get_ContainsFullScreenElement(&full); self->element_fullscreen(full != FALSE);
        }
        return S_OK;
      }).Get(), &fullscreen_token), "WebView2 fullscreen");
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
    }, [weak] {
      auto self = weak.lock();
      return self ? self->dock_label() : std::wstring();
    }, [weak] {
      if (auto self = weak.lock(); self && !self->closed_)
        if (!self->options_.on_reload || !self->options_.on_reload()) self->reload();
    }, [weak] {
      if (auto self = weak.lock(); self && !self->closed_)
        ShellExecuteW(self->hwnd_, L"open", self->options_.entry.parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }, [weak] {
      if (auto self = weak.lock(); self && !self->closed_ && self->options_.on_close) self->options_.on_close();
    }, [weak] {
      auto self = weak.lock();
      return self ? wide(self->options_.app_name ? self->options_.app_name() : self->options_.title) : std::wstring();
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
            if (text == WinCursor::message) self->cursor_.typed();
            else if (text == "__reawebPresent") self->prepare_startup();
            else if (page_background_message(text, [self](unsigned color) { self->set_background(color); })) {}
            else if (text.rfind("{\"__reawebNativeDrop\":", 0) == 0) self->native_drop(args, text);
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
        if (!self || self->closed_ || !same_document(utf8(uri), self->uri_)) {
          args->put_Cancel(TRUE);
          if (self && !self->closed_) blocked_navigation(utf8(uri), self->uri_, open_external,
            [self](const std::string& script) { self->evaluate(script); });
        }
        else if (self->options_.on_reload && self->options_.on_reload()) args->put_Cancel(TRUE);
        else {
          self->clear_video_surface();
          if (self->options_.hosted) self->set_visible(false);
          self->startup_.navigate(self->options_.hosted);
          self->cursor_.finish();
          if (self->options_.on_navigation) self->options_.on_navigation();
        }
        CoTaskMemFree(uri); return S_OK;
      }).Get(), &token), "add_NavigationStarting");
    check(webview_->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
      [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
        args->put_Cancel(TRUE);
        if (auto self = weak.lock(); self && !self->closed_) {
          LPWSTR uri = nullptr; args->get_Uri(&uri);
          const auto target = utf8(uri); CoTaskMemFree(uri);
          blocked_navigation(target, self->uri_, open_external,
            [self](const std::string& script) { self->evaluate(script); });
        }
        return S_OK;
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
        if (auto self = weak.lock()) self->fail("WEBVIEW_PROCESS_FAILED: WebView2 process failed; reopen the tool");
        return S_OK;
      }).Get(), &token), "add_ProcessFailed");
    check(webview_->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
      [weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
        BOOL success = FALSE; args->get_IsSuccess(&success);
        if (success) if (auto self = weak.lock()) self->mark_startup(StartupTiming::Loaded);
        if (!success) {
          COREWEBVIEW2_WEB_ERROR_STATUS status{}; args->get_WebErrorStatus(&status);
          if (status != COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED)
            if (auto self = weak.lock()) self->fail("WebView2 navigation failed: " + std::to_string(status));
        }
        return S_OK;
      }).Get(), &token), "add_NavigationCompleted");
    check(webview_->AddScriptToExecuteOnDocumentCreated(wide(options_.script +
      (options_.transparent ? transparent_page_script("chrome.webview.postMessage('__reawebPresent');") :
        page_background_script("chrome.webview.postMessage(message);")) + WinCursor::script +
      drag_regions_script("", true)).c_str(),
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
  std::shared_ptr<VideoMemory> video_memory(const std::string& token, uint32_t bytes) override {
    ComPtr<ICoreWebView2_17> view;
    ComPtr<ICoreWebView2_2> view2;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Environment12> modern;
    if (closed_ || !webview_ || FAILED(webview_.As(&view)) || FAILED(webview_.As(&view2)) ||
        FAILED(view2->get_Environment(&environment)) || FAILED(environment.As(&modern))) return {};
    struct Mapping final : VideoMemory {
      HANDLE handle = nullptr;
      ~Mapping() override { if (data) UnmapViewOfFile(data); if (handle) CloseHandle(handle); }
    };
    ComPtr<ICoreWebView2SharedBuffer> shared;
    const size_t size = size_t(bytes) * 2;
    if (FAILED(modern->CreateSharedBuffer(size, &shared))) return {};
    HANDLE handle = nullptr;
    auto mapping = std::make_shared<Mapping>();
    if (FAILED(shared->get_FileMappingHandle(&handle)) ||
        !DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &mapping->handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) return {};
    mapping->data = static_cast<unsigned char*>(MapViewOfFile(mapping->handle, FILE_MAP_WRITE, 0, 0, size));
    if (!mapping->data) return {};
    mapping->frame_bytes = bytes;
    const auto metadata = wide(Json{{"reawebVideo", token}}.dump());
    if (FAILED(view->PostSharedBufferToScript(shared.Get(), COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY, metadata.c_str()))) return {};
    return mapping;
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
    if (startup_.pending()) return false;
    auto root = hwnd_ ? GetAncestor(hwnd_, GA_ROOT) : nullptr;
    return hwnd_ && IsWindowVisible(hwnd_) && (!root || !IsIconic(root));
  }
  bool focused() const override {
    auto focus = GetFocus();
    return hwnd_ && (focus == hwnd_ || IsChild(hwnd_, focus));
  }
  void tick() override {
    update_video_surface();
    if (overlay_) overlay_->repair();
    if (!closed_ && startup_.expired()) { fail("WebView2 initial frame did not become available within 15 seconds"); return; }
    if (!closed_ && startup_.present()) {
      if (startup_region_) {
        if (!SetWindowRgn(hwnd_, nullptr, TRUE)) { fail("Cannot present the initial docked window"); return; }
        startup_region_ = false;
      } else if (!options_.hosted) {
        const BOOL cloaked = FALSE;
        if (FAILED(DwmSetWindowAttribute(hwnd_, DWMWA_CLOAK, &cloaked, sizeof(cloaked)))) {
          fail("WebView2 could not present the initial window"); return;
        }
      }
      if (maximized_) {
        WINDOWPLACEMENT placement{sizeof(placement)};
        GetWindowPlacement(hwnd_, &placement); placement.showCmd = SW_SHOWMAXIMIZED;
        SetWindowPlacement(hwnd_, &placement);
      }
      ShowWindow(hwnd_, SW_SHOWNA);
      // A controller created under a hidden HWND needs explicit first presentation,
      // even when the cached visibility is already true.
      if (controller_) { controller_->put_IsVisible(TRUE); visible_ = true; }
      RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
      if (!options_.hosted) mark_startup(StartupTiming::Revealed);
      if (startup_.focus_requested() && GetForegroundWindow() == startup_foreground_) {
        if (options_.on_present) options_.on_present();
        if (!(GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_CHILD)) focus();
      }
    }
    if (!closed_) cursor_.tick();
    if (!closed_) update_region();
    if (!closed_) resize_edges_.sync(hwnd_, controller_ && frameless_active() && resize_border());
    if (!closed_ && !options_.hosted) dock_redraw_.sync(hwnd_);
    if (!closed_ && !options_.hosted) icon_.refresh(hwnd_, icon_host());
    if (!closed_ && devtools_) devtools_->tick(webview_.Get());
    const bool next = startup_.pending() || visible();
    if (controller_ && next != visible_) { controller_->put_IsVisible(next); visible_ = next; }
  }
  bool video_presenter_available() const override { return !closed_ && controller_ && alpha_supported_ && desktop_composition(); }
  const char* video_presenter_backend() const override { return "d3d11"; }
  std::shared_ptr<VideoPresenter> video_presenter(const Json& descriptor) override {
    if (!video_presenter_available()) throw Error("VIDEO_PRESENTATION_UNAVAILABLE", "Native D3D presentation is unavailable");
    if (auto current = video_presenter_.lock(); current && !current->closed())
      throw Error("VIDEO_SURFACE_IN_USE", "This window already has a native video surface");
    clear_video_surface();
    if (!video_host_) video_host_ = make_win_video_host(hwnd_);
    auto presenter = make_win_video_presenter(video_host_, descriptor);
    ComPtr<ICoreWebView2Controller2> alpha; controller_.As(&alpha);
    if (FAILED(alpha->put_DefaultBackgroundColor({0,0,0,0}))) {
      presenter->invalidate(); throw Error("VIDEO_RENDERER_UNAVAILABLE", "Cannot enable video background transparency");
    }
    video_presenter_ = presenter; video_background_active_ = true;
    if (!update_video_clipping()) {
      clear_video_surface(); throw Error("VIDEO_RENDERER_UNAVAILABLE", "Cannot expose the native video layer");
    }
    update_video_surface();
    return presenter;
  }
  void focus() override {
    if (closed_ || !hwnd_ || startup_.cancelled()) return;
    if (startup_.pending()) { startup_.show(true); startup_foreground_ = GetForegroundWindow(); tick(); }
    if (closed_ || startup_.cancelled()) return;
    ShowWindow(hwnd_, SW_SHOWNA);
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
  HWND icon_host(bool showing = false) const {
    if (!(GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_CHILD) || (!showing && !IsWindowVisible(hwnd_))) return nullptr;
    const auto root = GetAncestor(hwnd_, GA_ROOT);
    return root != options_.parent && (GetWindowLongPtrW(root, GWL_STYLE) & WS_CAPTION) == WS_CAPTION ? root : nullptr;
  }
  void set_icon(const std::vector<IconBitmap>& images) override { icon_.set(hwnd_, images, icon_host()); }
  void clear_icon() override { icon_.clear(hwnd_, icon_host()); }
  void set_icon_visible(bool visible) override { icon_.set_visible(hwnd_, visible, icon_host()); }
  void set_visible(bool visible) override {
    startup_.request(visible);
    if (!visible) cursor_.finish();
    if (options_.hosted && controller_ && !startup_.pending()) {
      controller_->put_IsVisible(visible); visible_ = visible;
    }
    if (startup_.pending()) return;
    ShowWindow(hwnd_, visible ? SW_SHOWNA : SW_HIDE);
  }
  void show() override {
    if (closed_ || startup_.cancelled()) return;
    if (startup_.pending()) { startup_.show(); tick(); }
    else set_visible(true);
  }
  void without_focus(const std::function<void()>& action) override {
    const auto foreground = GetForegroundWindow(), focus = GetFocus();
    const auto restore = [&] {
      if (foreground && IsWindow(foreground) && GetForegroundWindow() != foreground) SetForegroundWindow(foreground);
      if (focus && IsWindow(focus) && GetFocus() != focus) SetFocus(focus);
    };
    try { action(); } catch (...) { restore(); throw; }
    restore();
  }
  bool visibility_requested() const override { return startup_.pending() ? startup_.wanted() : visible(); }
  bool presentation_pending() const override { return startup_.pending(); }
  void cancel_pending_show() override { startup_.cancel(); }
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
    return {{"backend", "WebView2"}, {"browserVersion", browser_version_}, {"environmentStartup", environment_timing_}, {"controllerPrewarmed", controller_prewarmed_}, {"controllerReady", controller_ != nullptr},
      {"controllerVisible", visible_}, {"devtools", devtools_->diagnostics()}, {"windowFeatures", window_capabilities()}};
  }
  bool frameless_active() const override { return options_.frameless && !closed_ && floating(); }
  void set_frameless(bool value) override {
    if (options_.frameless == value) return;
    if (value && !drag_regions_supported_) throw Error("HOST_UNAVAILABLE", "Frameless windows require WebView2 non-client region support");
    options_.frameless = value;
    if (!floating()) return;
    region_width_ = -1;
    configure_frame();
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    layout(); update_region();
    evaluate(value ? "window.__reawebDragRegionsActive?.(true);" : "window.__reawebDragRegionsActive?.(false);");
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
  }
  void set_corner_radius(double value) override { corner_radius_ = value; custom_region_ = true; region_width_ = -1; update_region(); }
  bool corner_radius_active() const override { return corner_radius_ > 0 && floating() && !IsZoomed(hwnd_) && !IsIconic(hwnd_); }
  bool layer_active() const override { return overlay_ && overlay_->layer_active(); }
  bool mouse_mode_active() const override { return !overlay_ || overlay_->mouse_active(); }
  void set_mouse_mode(const std::string& value) override {
    if (!overlay_) throw Error("WINDOW_HOSTED", "Mouse policies require a standalone App");
    overlay_->mode(value); options_.overlay.mode = value;
  }
  void set_mouse_regions(const std::vector<MouseRegion>& value) override {
    if (!overlay_) throw Error("WINDOW_HOSTED", "Mouse policies require a standalone App");
    overlay_->regions(value); options_.overlay.regions = value;
  }
  void clear_mouse_regions() override { if (overlay_) set_mouse_regions({}); }
  bool transparent_active() const override { return options_.transparent && alpha_ready_ && !closed_ && floating() && desktop_composition(); }
  Json window_capabilities() const override {
    return {{"overlay", overlay_capabilities(options_.hosted ? "Hosted JSFX editor" : !options_.parent ? "REAPER main window unavailable" : "", options_.hosted ? "Hosted JSFX editor" : "", "win32")},
      {"hiddenStart", true}, {"frameless", drag_regions_supported_}, {"setFrameless", drag_regions_supported_}, {"cornerRadius", {{"supported", true}, {"framed", true}, {"frameless", true}, {"max", 64}}}, {"resizable", true},
      {"transparent", true}, {"transparency", transparency_capabilities(alpha_supported_ && desktop_composition(), "dwm-hwnd",
        !desktop_composition() ? "DWM composition is unavailable" : !alpha_supported_ ? "WebView2 Controller2 is not ready or unavailable" : "", drag_regions_supported_)},
      {"dragRegions", drag_regions_supported_}, {"windowSystem", "win32"}};
  }
  Json placement() const override {
    if (startup_.pending() && !startup_.wanted() && floating_rect_.right > floating_rect_.left && floating_rect_.bottom > floating_rect_.top)
      return {{"x", floating_rect_.left}, {"y", floating_rect_.top},
        {"width", floating_rect_.right - floating_rect_.left}, {"height", floating_rect_.bottom - floating_rect_.top}, {"maximized", maximized_}};
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
    const bool shown = IsWindowVisible(hwnd_) != FALSE;
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
    if (shown && (IsZoomed(hwnd_) || IsIconic(hwnd_))) ShowWindow(hwnd_, SW_RESTORE);
    SetWindowPos(hwnd_, nullptr, rect.left, rect.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (shown) ShowWindow(hwnd_, maximized_ ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE);
  }
  void* native_handle() const override { return hwnd_; }
  unsigned background_color() const override { return (GetRValue(background_) << 16) | (GetGValue(background_) << 8) | GetBValue(background_); }
  void reparent_host(void* parent) override {
    if (!options_.hosted || closed_) throw Error("FX_STALE", "Hosted window is no longer available");
    if (!parent) {
      if (!hosted_parking_) hosted_parking_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"",
        WS_POPUP | WS_DISABLED, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      parent = hosted_parking_;
    }
    if (!IsWindow(static_cast<HWND>(parent))) throw Error("FX_STALE", "Hosted parent is no longer available");
    if (GetParent(hwnd_) == parent) return;
    set_visible(false);
    ShowWindow(hwnd_, SW_HIDE);
    SetParent(hwnd_, static_cast<HWND>(parent));
    if (GetParent(hwnd_) != parent) throw Error("HOST_UNAVAILABLE", "Cannot reparent the hosted window");
    options_.parent = parent;
    if (controller_) controller_->NotifyParentWindowPositionChanged();
    layout();
    // Keep composing behind the empty startup region until CapturePreview completes.
    if (parent != hosted_parking_ && startup_.pending() && startup_region_) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  }
  void prepare_dock() override {
    if (options_.overlay.floating_only()) throw Error("DOCK_UNAVAILABLE", "Overlay and mouse policies require a floating window");
    if (options_.transparent) throw Error("DOCK_UNAVAILABLE", "Transparent windows require a standalone floating host");
    if (options_.frameless) evaluate("window.__reawebDragRegionsActive?.(false);");
    if (!startup_region_) SetWindowRgn(hwnd_, nullptr, TRUE);
    region_width_ = -1;
    if (startup_.pending() && !startup_region_) {
      // DWM cannot uncloak an HWND after it becomes a non-layered child.
      auto region = CreateRectRgn(0, 0, 0, 0);
      if (!region || !SetWindowRgn(hwnd_, region, FALSE)) {
        if (region) DeleteObject(region);
        throw std::runtime_error("Cannot prepare the initial docked window");
      }
      startup_region_ = true;
      const BOOL cloaked = FALSE;
      check(DwmSetWindowAttribute(hwnd_, DWMWA_CLOAK, &cloaked, sizeof(cloaked)), "Prepare initial docking");
    }
    const auto saved = placement();
    maximized_ = saved.value("maximized", false);
    if (maximized_) ShowWindow(hwnd_, SW_RESTORE);
    GetWindowRect(hwnd_, &floating_rect_);
  }
  void restore_floating() override {
    SetParent(hwnd_, nullptr);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, floating_style());
    configure_frame();
    SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(options_.parent));
    // Clearing the startup region while docked leaves DWM non-client rendering disabled.
    // Refresh it as a top-level window, unless the first frame still needs the empty region.
    if (!startup_region_) SetWindowRgn(hwnd_, nullptr, FALSE);
    region_width_ = -1;
    icon_.refresh(hwnd_);
    restore_placement({{"x", floating_rect_.left}, {"y", floating_rect_.top},
      {"width", floating_rect_.right - floating_rect_.left}, {"height", floating_rect_.bottom - floating_rect_.top}, {"maximized", maximized_}});
    if (startup_.pending() && startup_.wanted()) ShowWindow(hwnd_, SW_SHOWNA);
    if (controller_) controller_->NotifyParentWindowPositionChanged();
    if (options_.frameless) evaluate("window.__reawebDragRegionsActive?.(true);");
    update_region();
    if (!startup_.pending()) {
      if (maximized_) ShowWindow(hwnd_, SW_SHOWMAXIMIZED);
      focus();
    }
  }
};

struct EnvironmentState {
  StartupTiming::Clock::time_point started = StartupTiming::Clock::now(), completed;
  enum class Status { NotStarted, Initializing, Ready, Failed };
  Status status = Status::NotStarted;
  uint64_t attempt = 0;
  ComPtr<ICoreWebView2Environment> environment;
  std::vector<std::weak_ptr<WinWindow>> waiting;
  std::string error;
};
class WinPlatform final : public Platform {
  IdlePool<WinIdleView> idle_;
  std::shared_ptr<EnvironmentState> state_ = std::make_shared<EnvironmentState>();
  HINSTANCE instance_ = nullptr;
  bool com_ = false;
  HWND clipboard_owner_ = nullptr;
  fs::path data_;
  void prepare_environment(bool retry = false) {
    if (state_->status == EnvironmentState::Status::Ready || state_->status == EnvironmentState::Status::Initializing ||
        (state_->status == EnvironmentState::Status::Failed && !retry)) return;
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    auto scheme = Microsoft::WRL::Make<CoreWebView2CustomSchemeRegistration>(L"reaweb");
    check(scheme->put_HasAuthorityComponent(TRUE), "App scheme authority");
    check(scheme->put_TreatAsSecure(TRUE), "App secure scheme");
    const wchar_t* origins[] = {L"reaweb://*"};
    check(scheme->SetAllowedOrigins(1, origins), "App scheme origins");
    ICoreWebView2CustomSchemeRegistration* schemes[] = {scheme.Get()};
    check(options->SetCustomSchemeRegistrations(1, schemes), "Register App scheme");
    // Keep GPU compositing and WebGL, but avoid cold GPU raster pipeline stalls.
    std::wstring arguments = L"--disable-gpu-rasterization";
    if (startup_color_profile == ColorProfile::SRGB) arguments += L" --force-color-profile=srgb";
    check(options->put_AdditionalBrowserArguments(arguments.c_str()), "WebView2 rendering options");
    const auto attempt = ++state_->attempt;
    state_->status = EnvironmentState::Status::Initializing;
    state_->error.clear();
    state_->started = StartupTiming::Clock::now();
    std::weak_ptr<EnvironmentState> weak = state_;
    auto hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, data_.c_str(), options.Get(),
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [weak, attempt](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
          auto state = weak.lock();
          if (!state || state->attempt != attempt || state->status != EnvironmentState::Status::Initializing) return S_OK;
          state->completed = StartupTiming::Clock::now();
          if (FAILED(result) || !environment) state->error = "WebView2 initialization failed. Install Microsoft Edge WebView2 Evergreen Runtime. HRESULT " + std::to_string(static_cast<unsigned long>(result));
          else state->environment = environment;
          state->status = state->error.empty() ? EnvironmentState::Status::Ready : EnvironmentState::Status::Failed;
          auto waiting = std::move(state->waiting);
          for (auto& item : waiting) if (auto window = item.lock(); window && !window->environment_wait_cancelled()) {
            try {
              if (!state->error.empty()) window->fail(state->error);
              else window->initialize(environment, {{"mode", "waiting"}, {"initializationMs",
                std::chrono::duration<double, std::milli>(state->completed - state->started).count()}});
            } catch (const std::exception& e) { window->fail(e.what()); }
          }
          return S_OK;
        }).Get());
    if (FAILED(hr)) {
      state_->status = EnvironmentState::Status::Failed;
      state_->completed = StartupTiming::Clock::now();
      state_->error = "WebView2 Runtime is unavailable. Install Microsoft Edge WebView2 Evergreen Runtime. HRESULT " + std::to_string(static_cast<unsigned long>(hr));
    }
  }
public:
  explicit WinPlatform(const fs::path& data) : data_(data) {
    check(OleInitialize(nullptr), "OleInitialize (WebView2 and native drag require STA)");
    com_ = true;
    // COM completion handlers can outlive extension teardown. Keep their code mapped until process exit.
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(&WinWindow::proc), &instance_);
    WNDCLASSW cls{}; cls.lpfnWndProc = WinWindow::proc; cls.hInstance = instance_;
    cls.lpszClassName = window_class; cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      OleUninitialize(); com_ = false; throw std::runtime_error("RegisterClass failed");
    }
    try { prepare_environment(); }
    catch (...) { UnregisterClassW(window_class, instance_); OleUninitialize(); com_ = false; throw; }
  }
  ~WinPlatform() override {
    idle_.capacity(0);
    if (clipboard_owner_) DestroyWindow(clipboard_owner_);
    state_.reset(); UnregisterClassW(window_class, instance_);
    if (com_) OleUninitialize();
  }
  void desktop(const std::string& method, const Json& args, DesktopReply reply) override {
    if (method == "ReaWeb_GetDisplays") {
      Json displays = Json::array();
      EnumDisplayMonitors(nullptr, nullptr, +[](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
        MONITORINFOEXW info{}; info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info))) return TRUE;
        UINT x = 96, y = 96;
        auto library = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (library) {
          auto dpi = reinterpret_cast<HRESULT (WINAPI*)(HMONITOR, int, UINT*, UINT*)>(GetProcAddress(library, "GetDpiForMonitor"));
          if (dpi) dpi(monitor, 0, &x, &y); FreeLibrary(library);
        }
        const auto rectangle = [](const RECT& r) { return Json{{"x", r.left}, {"y", r.top}, {"width", r.right - r.left}, {"height", r.bottom - r.top}}; };
        reinterpret_cast<Json*>(context)->push_back({{"id", utf8(info.szDevice)}, {"bounds", rectangle(info.rcMonitor)},
          {"workArea", rectangle(info.rcWork)}, {"scaleFactor", x / 96.0}, {"dpi", x}, {"primary", (info.dwFlags & MONITORINFOF_PRIMARY) != 0}, {"units", "native"}});
        return TRUE;
      }, reinterpret_cast<LPARAM>(&displays));
      reply({{"result", displays}}); return;
    }
    if (method == "ReaWeb_RevealPath") {
      auto id = ILCreateFromPathW(wide(args.at(0).get<std::string>()).c_str());
      if (!id) throw Error("INVALID_PATH", "Cannot locate file in Explorer");
      const auto result = SHOpenFolderAndSelectItems(id, 0, nullptr, 0); CoTaskMemFree(id);
      if (FAILED(result)) throw Error("REVEAL_FAILED", "Explorer could not reveal the path");
      reply({{"result", true}}); return;
    }
    if (method == "ReaWeb_OpenExternal") {
      open_external(args[0].get<std::string>());
      reply({{"result", true}}); return;
    }
    if (!clipboard_owner_) clipboard_owner_ = CreateWindowExW(0, L"STATIC", L"ReaWebAPI Clipboard", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance_, nullptr);
    if (!clipboard_owner_ || !OpenClipboard(clipboard_owner_)) throw Error("CLIPBOARD_BUSY", "The clipboard is busy; try again");
    struct Close { ~Close() { CloseClipboard(); } } close;
    if (method == "ReaWeb_ClipboardReadBinary" || method == "ReaWeb_ClipboardWriteBinary") {
      const auto format = RegisterClipboardFormatW(wide("ReaWebAPI.Binary:" + args.at(0).get<std::string>()).c_str());
      if (!format) throw Error("CLIPBOARD_ERROR", "Cannot register binary clipboard format");
      if (method == "ReaWeb_ClipboardReadBinary") {
        auto handle = GetClipboardData(format); if (!handle) { reply({{"result", nullptr}}); return; }
        const auto bytes = GlobalSize(handle); auto* data = static_cast<const char*>(GlobalLock(handle));
        if (!data) throw Error("CLIPBOARD_ERROR", "Cannot read clipboard data");
        uint64_t size = 0; if (bytes >= 8) std::memcpy(&size, data, 8);
        if (bytes < 8 || size > value_limit || size > bytes - 8) { GlobalUnlock(handle); throw Error("CLIPBOARD_ERROR", "Invalid binary clipboard data"); }
        auto result = encode_binary(data + 8, static_cast<size_t>(size)); GlobalUnlock(handle); reply({{"result", result}}); return;
      }
      const auto bytes = decode_binary(args.at(1)); auto handle = GlobalAlloc(GMEM_MOVEABLE, bytes.size() + 8);
      if (!handle) throw Error("CLIPBOARD_ERROR", "Cannot allocate binary clipboard data");
      auto* data = static_cast<char*>(GlobalLock(handle));
      if (!data) { GlobalFree(handle); throw Error("CLIPBOARD_ERROR", "Cannot lock clipboard data"); }
      const uint64_t length = bytes.size(); std::memcpy(data, &length, 8); std::memcpy(data + 8, bytes.data(), bytes.size()); GlobalUnlock(handle);
      if (!EmptyClipboard() || !SetClipboardData(format, handle)) { GlobalFree(handle); throw Error("CLIPBOARD_ERROR", "Cannot write binary clipboard data"); }
      reply({{"result", true}}); return;
    }
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
    prepare_environment(true);
    const bool ready = state_->status == EnvironmentState::Status::Ready;
    if (!state_->error.empty()) throw std::runtime_error(state_->error);
    if (options.startup_timing) options.startup_timing->mark(StartupTiming::EnvironmentWait);
    auto window = std::make_shared<WinWindow>(std::move(options), instance_);
    auto prepared = idle_.take();
    if (state_->environment) window->initialize(state_->environment.Get(), {{"mode", ready ? "reused" : "waiting"}, {"initializationMs",
      std::chrono::duration<double, std::milli>(state_->completed - state_->started).count()}},
      prepared ? prepared->attach(static_cast<HWND>(window->native_handle())) : ComPtr<ICoreWebView2Controller>{});
    else state_->waiting.push_back(window);
    return window;
  }
  void set_idle_capacity(unsigned count) override { idle_.capacity(count); }
  Json idle_diagnostics() const override { return idle_.diagnostics(); }
  void idle() override {
    if (state_->status != EnvironmentState::Status::Ready) return;
    idle_.maintain([this] {
      auto view = std::make_shared<WinIdleView>(instance_);
      view->start(state_->environment.Get());
      return view;
    });
  }
};
}
std::unique_ptr<Platform> make_platform(const fs::path& data) { return std::make_unique<WinPlatform>(data); }
}
