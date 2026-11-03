// Opt-in native teardown acceptance. Opens only disposable test windows.
#include "../src/platform/windows/platform_win.cpp"
#include <fstream>
#include <iostream>

using namespace reaweb;
#define CHECK(v) do { if (!(v)) throw std::runtime_error(#v); } while (false)
namespace {
HWND closing_window = nullptr;
int destroyed_children = 0;
bool exposed = false;
LRESULT CALLBACK observe(int code, WPARAM wp, LPARAM lp) {
  if (code >= 0 && closing_window) {
    const auto& message = *reinterpret_cast<CWPSTRUCT*>(lp);
    if (message.message == WM_DESTROY &&
        (message.hwnd == closing_window || IsChild(closing_window, message.hwnd))) {
      exposed |= IsWindowVisible(closing_window) != FALSE;
      if (message.hwnd != closing_window) ++destroyed_children;
    }
  }
  return CallNextHookEx(nullptr, code, wp, lp);
}
}
int main() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  try {
    const auto path = fs::current_path() / ".cache" / ("close-win-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(path);
    std::ofstream(path / "index.html") << "<!doctype html><style>html{background:#182838}</style><input value=Close>";
    auto platform = make_platform(path / "profile");
    const auto hook = SetWindowsHookExW(WH_CALLWNDPROC, observe, nullptr, GetCurrentThreadId());
    CHECK(hook);
    struct Unhook { HHOOK hook; ~Unhook() { UnhookWindowsHookEx(hook); } } unhook{hook};
    for (int repeat = 0; repeat < 5; ++repeat) for (int mode = 0; mode < 6; ++mode) {
      std::string error;
      WindowOptions options; options.entry = path / "index.html"; options.title = "Close acceptance";
      options.frameless = mode != 1; options.hidden = mode == 2 || mode == 3;
      options.resizable = mode != 4;
      options.on_error = [&](std::string message) { error = message; };
      auto window = platform->open(std::move(options)); window->show_after_create();
      const auto hwnd = static_cast<HWND>(window->native_handle());
      if (mode != 3) {
        const auto deadline = GetTickCount64() + 15000;
        while (!window->diagnostics().value("controllerReady", false) || (mode != 2 && !window->visible())) {
          MSG message{};
          while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
          platform->pump(); window->tick();
          CHECK(error.empty()); CHECK(GetTickCount64() < deadline); Sleep(2);
        }
        if (mode == 5) { window->set_corner_radius(16); ShowWindow(hwnd, SW_MAXIMIZE); }
        CHECK(bool(IsWindowVisible(hwnd)) == (mode != 2));
      }
      closing_window = hwnd; destroyed_children = 0; exposed = false;
      window.reset(); closing_window = nullptr;
      CHECK(!IsWindow(hwnd));
      CHECK(!exposed);
      if (mode != 3) CHECK(destroyed_children > 0);
      std::cout << "PASS: hide before child teardown, repeat=" << repeat << " mode=" << mode << '\n';
    }
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
