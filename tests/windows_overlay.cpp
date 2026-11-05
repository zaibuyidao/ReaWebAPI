#include "../src/platform/windows/platform_win.cpp"
#include <fstream>
#include <iostream>
using namespace reaweb;
#define CHECK(v) do { if (!(v)) throw std::runtime_error(#v); } while (false)
static int presses = 0, releases = 0, wheels = 0;
static LRESULT CALLBACK background_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) { ++presses; SetCapture(hwnd); return 0; }
  if (message == WM_LBUTTONUP || message == WM_RBUTTONUP) { ++releases; ReleaseCapture(); return 0; }
  if (message == WM_MOUSEWHEEL) { ++wheels; return 0; }
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{}; auto dc = BeginPaint(hwnd, &paint);
    auto brush = CreateSolidBrush(RGB(20, 80, 180)); FillRect(dc, &paint.rcPaint, brush); DeleteObject(brush);
    EndPaint(hwnd, &paint); return 0;
  }
  return DefWindowProcW(hwnd, message, wp, lp);
}
int main() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INPUT reset{}; reset.type = INPUT_MOUSE; reset.mi.dwFlags = MOUSEEVENTF_LEFTUP | MOUSEEVENTF_RIGHTUP;
  SendInput(1, &reset, sizeof(reset));
  POINT saved{}; GetCursorPos(&saved);
  struct Restore { POINT point; ~Restore() { INPUT input{}; input.type = INPUT_MOUSE; input.mi.dwFlags = MOUSEEVENTF_LEFTUP | MOUSEEVENTF_RIGHTUP; SendInput(1, &input, sizeof(input)); SetCursorPos(point.x, point.y); } } restore{saved};
  try {
    const auto path = fs::current_path() / ".cache" / ("overlay-win-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(path);
    std::ofstream(path / "index.html") << R"(<!doctype html><style>html,body{background:transparent;margin:0}button{position:absolute;left:100px;top:100px;width:200px;height:160px;background:#f008}</style><button>Drag</button><script>
      for(const type of ['pointerdown','pointerup']) addEventListener(type,e=>{chrome.webview.postMessage(type);if(type==='pointerdown') e.target.setPointerCapture(e.pointerId)});
    </script>)";
    auto platform = make_platform(path / "profile");
    WNDCLASSW cls{}; cls.lpfnWndProc = background_proc; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"OverlayBackground";
    RegisterClassW(&cls);
    auto host = CreateWindowExW(0, cls.lpszClassName, L"Overlay native background", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
      50, 50, 1000, 750, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    struct Cleanup { HWND value; ~Cleanup() { DestroyWindow(value); } } cleanup{host};
    SetForegroundWindow(host);
    std::string error; int web_down = 0, web_up = 0;
    WindowOptions options; options.entry = path / "index.html"; options.title = "Overlay acceptance";
    options.parent = host; options.transparent = true; options.frameless = true;
    options.overlay = {"overlay", "regions", {{100, 100, 200, 160}}};
    options.on_message = [&](std::string message) { if (message == "pointerdown") ++web_down; if (message == "pointerup") ++web_up; };
    options.on_error = [&](std::string value) { error = value; };
    auto window = platform->open(std::move(options));
    window->restore_placement({{"x", 150}, {"y", 150}, {"width", 600}, {"height", 420}});
    window->show_after_create();
    auto hwnd = static_cast<HWND>(window->native_handle());
    std::shared_ptr<Window> second;
    const auto pump = [&](unsigned duration) {
      const auto start = GetTickCount64();
      do {
        MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        platform->pump(); window->tick(); if (second) second->tick(); CHECK(error.empty()); Sleep(2);
      } while (GetTickCount64() - start < duration);
    };
    const auto mouse = [&](DWORD flags, DWORD data = 0) {
      INPUT input{}; input.type = INPUT_MOUSE; input.mi.dwFlags = flags; input.mi.mouseData = data;
      CHECK(SendInput(1, &input, sizeof(input)) == 1); pump(120);
    };
    pump(2500); CHECK(window->transparent_active()); CHECK(window->layer_active()); CHECK(window->mouse_mode_active());
    POINT origin{}; ClientToScreen(hwnd, &origin);
    const double dpi = GetDpiForWindow(hwnd) / 96.0;
    const auto move = [&](int x, int y) { SetCursorPos(origin.x + int(x * dpi), origin.y + int(y * dpi)); };
    move(40, 40); pump(150);
    auto screen = GetDC(nullptr); const auto pixel = GetPixel(screen, origin.x + 40, origin.y + 40); ReleaseDC(nullptr, screen);
    std::cout << "passthrough clear pixel=" << pixel << " expected=" << RGB(20,80,180) << '\n';
    CHECK(pixel == RGB(20,80,180));
    const auto before = presses;
    mouse(MOUSEEVENTF_LEFTDOWN); move(170, 140); pump(80); mouse(MOUSEEVENTF_LEFTUP);
    CHECK(presses == before + 1); CHECK(releases == presses); CHECK(!web_down);
    move(160, 140); pump(100);
    mouse(MOUSEEVENTF_LEFTDOWN); pump(1000); CHECK(web_down == 1);
    window->set_mouse_regions({}); window->set_mouse_mode("passthrough");
    CHECK(!window->mouse_mode_active());
    move(40, 40); pump(100); mouse(MOUSEEVENTF_LEFTUP);
    CHECK(web_up == 1); CHECK(presses == before + 1); CHECK(window->mouse_mode_active());
    mouse(MOUSEEVENTF_RIGHTDOWN); mouse(MOUSEEVENTF_RIGHTUP); CHECK(presses == before + 2);
    mouse(MOUSEEVENTF_WHEEL, WHEEL_DELTA); CHECK(wheels > 0);
    window->set_mouse_mode("normal"); pump(100);
    mouse(MOUSEEVENTF_LEFTDOWN); mouse(MOUSEEVENTF_LEFTUP); CHECK(web_down == 2); CHECK(web_up == 2);
    auto floating = CreateWindowExW(0, cls.lpszClassName, L"Floating native window", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
      350, 250, 200, 200, host, nullptr, GetModuleHandleW(nullptr), nullptr);
    Cleanup float_cleanup{floating}; pump(100);
    const auto order = [&] {
      bool seen_float = false, seen_overlay = false;
      for (auto w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (w == floating) seen_float = true;
        if (w == hwnd) { CHECK(seen_float); seen_overlay = true; }
        if (w == host) { CHECK(seen_overlay); break; }
      }
    };
    order(); window->focus(); pump(200); order(); SetForegroundWindow(host); pump(200); order();
    window->set_visible(false); CHECK(window->layer_active()); window->show(); pump(200); order();
    window->set_mouse_mode("regions"); window->set_mouse_regions({{0, 0, 60, 60}}); pump(100);
    mouse(MOUSEEVENTF_LEFTDOWN); mouse(MOUSEEVENTF_LEFTUP); CHECK(web_down == 3);
    WindowOptions another; another.entry = path / "index.html"; another.parent = host;
    another.title = "Second overlay"; another.overlay.layer = "overlay";
    another.on_message = [](std::string) {}; another.on_error = [&](std::string value) { error = value; };
    second = platform->open(std::move(another)); second->show_after_create(); pump(1500);
    second->focus(); pump(150); order(); window->focus(); pump(150); order();
    bool found_second = false;
    for (auto w=GetTopWindow(nullptr);w;w=GetWindow(w,GW_HWNDNEXT)) {
      if(w==second->native_handle()) found_second=true;
      if(w==host) { CHECK(found_second); break; }
    }
    second.reset(); pump(100); order();
    std::cout << "PASS: native pass-through, visual Alpha, regions, captured drag, deferred policy, wheel/right click, transparent controls and ordering\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
