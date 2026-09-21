#include "platform/platform.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <chrono>
using namespace reaweb;
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed: " #value); } while (false)
void pump(const std::vector<std::shared_ptr<Window>>& windows, const std::function<bool()>& done, const char* stage = "DevTools") {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  do {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    for (auto& window : windows) window->tick();
    if (done()) return;
    Sleep(5);
  } while (std::chrono::steady_clock::now() < deadline);
  for (auto& window : windows) std::cerr << window->diagnostics().dump() << '\n';
  throw std::runtime_error(std::string(stage) + " test timed out");
}
void shortcut(const std::shared_ptr<Window>& window, bool host = false) {
  // Exercise the native shortcut without injecting input into the user's desktop.
  if (!host) window->focus();
  auto root = static_cast<HWND>(window->native_handle());
  auto target = host ? root : GetFocus();
  CHECK(target && (host || (target != root && IsChild(root, target))));
  BYTE saved[256]{}, keys[256]{}; CHECK(GetKeyboardState(saved));
  memcpy(keys, saved, sizeof(keys)); keys[VK_CONTROL] = keys[VK_SHIFT] = 0x80; keys[VK_MENU] = 0;
  CHECK(SetKeyboardState(keys));
  SendMessageW(target, WM_KEYDOWN, 'I', 0);
  SendMessageW(target, WM_KEYDOWN, 'I', 1LL << 30); // Repeat must be ignored.
  CHECK(SetKeyboardState(saved));
}
HWND inspector_for(const std::shared_ptr<Window>& window) {
  struct Search { HWND owner, result; } search{static_cast<HWND>(window->native_handle()), nullptr};
  EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
    auto visit = [](HWND child, LPARAM data) -> BOOL {
      auto& search = *reinterpret_cast<Search*>(data);
      if (GetPropW(child, L"ReaWebAPI.DevTools.Owner") &&
          GetPropW(child, L"ReaWebAPI.DevTools.ToggleTarget") == search.owner) search.result = child;
      return TRUE;
    };
    visit(hwnd, data); EnumChildWindows(hwnd, visit, data);
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.result;
}
bool fills_panel(HWND panel, HWND inspector) {
  CHECK(!FindWindowExW(panel, nullptr, L"BUTTON", nullptr));
  auto renderer = FindWindowExW(inspector, nullptr, L"Chrome_RenderWidgetHostHWND", nullptr);
  if (!renderer) return false;
  RECT content{}, bounds{}; GetClientRect(panel, &content);
  MapWindowPoints(panel, nullptr, reinterpret_cast<POINT*>(&content), 2);
  GetWindowRect(renderer, &bounds);
  return EqualRect(&content, &bounds);
}
void embedded_decorations(HWND panel, HWND inspector) {
  CHECK(!(GetWindowLongPtrW(inspector, GWL_STYLE) & (WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)));
  CHECK(!(GetWindowLongPtrW(inspector, GWL_EXSTYLE) & (WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE)));
  RECT rect{}; GetWindowRect(panel, &rect);
  CHECK(SendMessageW(inspector, WM_NCHITTEST, 0, MAKELPARAM((rect.left + rect.right) / 2, rect.top + 1)) == HTCLIENT);
}
int browser_windows(HWND inspector) {
  struct Search { DWORD process = 0; int count = 0; } search;
  GetWindowThreadProcessId(inspector, &search.process);
  EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
    auto& search = *reinterpret_cast<Search*>(data);
    DWORD process = 0; GetWindowThreadProcessId(hwnd, &process);
    wchar_t name[128]{}; GetClassNameW(hwnd, name, 128);
    if (process == search.process && !wcscmp(name, L"Chrome_WidgetWin_1")) ++search.count;
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.count;
}
void inspector_shortcut(HWND inspector, const std::vector<std::shared_ptr<Window>>& windows) {
  static int attempt = 0;
  const auto stage = "Inspector focus " + std::to_string(++attempt);
  const auto thread = GetCurrentThreadId();
  const auto foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
  const bool attached = foreground != thread && AttachThreadInput(thread, foreground, TRUE);
  if (foreground != thread && !attached && GetLastError() == ERROR_ACCESS_DENIED)
    throw std::runtime_error("Foreground input is unavailable; run the shortcut test on an unlocked interactive desktop");
  auto root = GetAncestor(inspector, GA_ROOT);
  SetForegroundWindow(root); SetFocus(inspector);
  if (attached) AttachThreadInput(thread, foreground, FALSE);
  pump(windows, [&] { return GetForegroundWindow() == root; }, stage.c_str());
  SetFocus(inspector);
  INPUT keys[7]{};
  const WORD codes[] = {VK_CONTROL, VK_SHIFT, 'I', 'I', 'I', VK_SHIFT, VK_CONTROL};
  for (int i = 0; i < 7; ++i) {
    keys[i].type = INPUT_KEYBOARD; keys[i].ki.wVk = codes[i];
    if (i >= 4) keys[i].ki.dwFlags = KEYEVENTF_KEYUP;
  }
  CHECK(!(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000));
  struct ReleaseKeys {
    INPUT* keys;
    ~ReleaseKeys() { if (keys) SendInput(2, keys + 5, sizeof(INPUT)); }
  } release{keys};
  CHECK(SendInput(2, keys, sizeof(INPUT)) == 2);
  pump(windows, [] { return (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000); }, "Shortcut modifiers down");
  CHECK(GetForegroundWindow() == root);
  CHECK(SendInput(5, keys + 2, sizeof(INPUT)) == 5);
  pump(windows, [] { return !(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000); }, "Shortcut modifiers up");
  release.keys = nullptr;
}
LRESULT CALLBACK shortcut_probe(HWND hwnd, UINT message, WPARAM key, LPARAM data) {
  if (message == WM_NCCREATE)
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(data)->lpCreateParams));
  if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && key == 'I')
    *reinterpret_cast<int*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)) += LOWORD(data);
  return DefWindowProcW(hwnd, message, key, data);
}
int main(int argc, char** argv) {
  const bool dpi_v1 = argc > 1 && std::string(argv[1]) == "--dpi-v1";
  SetProcessDpiAwarenessContext(dpi_v1 ? DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE : DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  try {
    auto path = fs::current_path() / ("devtools-test-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(path);
    auto entry = path / "index.html";
    std::ofstream(entry) << "<!doctype html><h1>DevTools test</h1>";
    auto platform = make_platform(path / "profile");
    int messages = 0, navigations = 0, page_width = 0;
    std::string error;
    WindowOptions options;
    bool docked = false;
    options.on_dock_toggle = [&] { docked = !docked; };
    options.is_docked = [&] { return docked; };
    options.entry = entry; options.title = "ReaWebAPI DevTools test";
    options.script = "window.token='retained';setInterval(()=>chrome.webview.postMessage(window.token),50);console.log('retained console entry');";
    options.on_message = [&](std::string text) {
      if (text.rfind("width:", 0) == 0) { page_width = std::stoi(text.substr(6)); return; }
      CHECK(text == "retained"); ++messages;
    };
    options.on_error = [&](std::string message) { error = message; };
    options.on_navigation = [&] { ++navigations; };
    auto first = platform->open(options);
    std::vector<std::shared_ptr<Window>> windows{first};
    auto visible = [](const auto& window) { return window->diagnostics()["devtools"]["visible"].template get<bool>(); };
    pump(windows, [&] { return messages > 0; }, "Initial page heartbeat");
    auto native = static_cast<HWND>(first->native_handle());
    CHECK(!GetDlgItem(native, 0x1800));
    CHECK(GetMenuState(GetSystemMenu(native, FALSE), 0x1800, MF_BYCOMMAND) != UINT(-1));
    SendMessageW(native, WM_SYSCOMMAND, 0x1800, 0); first->tick(); CHECK(docked);
    SendMessageW(native, WM_SYSCOMMAND, 0x1800, 0); first->tick(); CHECK(!docked);
    CHECK(navigations == 1);
    shortcut(first);
    pump(windows, [&] { return first->diagnostics()["devtools"]["pending"].get<bool>(); }, "Pending open");
    shortcut(first, true);
    pump(windows, [&] { return !first->diagnostics()["devtools"]["pending"].get<bool>(); }, "Cancelled open");
    CHECK(!visible(first));
    shortcut(first); pump(windows, [&] { return visible(first); }, "First embedded open");
    auto inspector = inspector_for(first);
    CHECK(GetPropW(inspector, L"ReaWebAPI.DevTools.Owner"));
    if (first->diagnostics()["devtools"]["embeddedSupported"] != true)
      std::cerr << first->diagnostics().dump() << '\n';
    CHECK(first->diagnostics()["devtools"]["embeddedSupported"] == true);
    CHECK(first->diagnostics()["devtools"]["mode"] == "embedded");
    CHECK(IsChild(native, inspector) && (GetWindowLongPtrW(inspector, GWL_STYLE) & WS_CHILD));
    auto panel = FindWindowExW(native, nullptr, L"ReaWebAPI.DevTools.Panel", L"Developer Tools");
    CHECK(panel);
    CHECK(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(inspector), GetWindowDpiAwarenessContext(GetParent(inspector))));
    pump(windows, [&] { return fills_panel(panel, inspector); }, "Embedded renderer fills panel");
    embedded_decorations(panel, inspector);
    auto divider = FindWindowExW(native, nullptr, L"ReaWebAPI.DevTools.Splitter", nullptr);
    CHECK(divider && IsWindowVisible(divider));
    RECT divider_rect{}; GetWindowRect(divider, &divider_rect);
    CHECK(divider_rect.right - divider_rect.left == 1);
    CHECK(reinterpret_cast<HBRUSH>(GetClassLongPtrW(divider, GCLP_HBRBACKGROUND)) == GetSysColorBrush(COLOR_WINDOWFRAME));
    RECT client{}, panel_rect{}; GetClientRect(native, &client); GetWindowRect(panel, &panel_rect);
    CHECK(std::abs(static_cast<double>(panel_rect.right - panel_rect.left) / client.right - 0.4) < 0.02);
    SendMessageW(divider, WM_LBUTTONDOWN, MK_LBUTTON, 0);
    SendMessageW(divider, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(-90, 4));
    SendMessageW(divider, WM_LBUTTONUP, 0, 0);
    const auto ratio = first->devtools_state()["widthRatio"].get<double>();
    CHECK(ratio > 0.4 && ratio < 0.8);
    SetWindowPos(native, nullptr, 0, 0, 1000, 700, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    pump(windows, [&] { return fills_panel(panel, inspector); }, "Resized renderer fills panel");
    CHECK(first->devtools_state()["widthRatio"] == ratio);
    auto measure_page = [&] {
      page_width = 0; first->evaluate("chrome.webview.postMessage('width:'+innerWidth)");
      pump(windows, [&] { return page_width > 0; }); return page_width;
    };
    const auto embedded_width = measure_page();
    RECT floating_bounds{};
    for (int i = 0; i < 3; ++i) {
      first->restore_devtools({{"mode", "floating"}});
      CHECK(first->diagnostics()["devtools"]["mode"] == "floating");
      auto floating = GetAncestor(inspector, GA_ROOT);
      CHECK(floating == inspector && GetWindow(floating, GW_OWNER) == native);
      CHECK(!(GetWindowLongPtrW(inspector, GWL_STYLE) & WS_CHILD));
      CHECK((GetWindowLongPtrW(inspector, GWL_STYLE) & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW);
      CHECK(!IsWindowVisible(panel));
      if (!i) {
        SetWindowPos(inspector, nullptr, 180, 120, 700, 500, SWP_NOZORDER | SWP_NOACTIVATE);
        GetWindowRect(inspector, &floating_bounds);
      } else {
        RECT rect{}; GetWindowRect(inspector, &rect); CHECK(EqualRect(&rect, &floating_bounds));
      }
      CHECK(!IsWindowVisible(divider) && inspector_for(first) == inspector);
      CHECK(measure_page() > embedded_width * 1.5);
      shortcut(first);
      pump(windows, [&] { return !visible(first); });
      CHECK(IsWindow(inspector));
      first->devtools(); pump(windows, [&] { return visible(first); });
      first->restore_devtools({{"mode", "embedded"}});
      CHECK(first->diagnostics()["devtools"]["mode"] == "embedded");
      CHECK(IsChild(native, inspector) && IsWindowVisible(divider) && inspector_for(first) == inspector);
      pump(windows, [&] { return fills_panel(panel, inspector); }, "Reembedded renderer fills panel");
      embedded_decorations(panel, inspector);
      CHECK(first->devtools_state()["widthRatio"] == ratio);
    }
    first->restore_devtools({{"mode", "floating"}});
    ShowWindow(inspector, SW_MAXIMIZE);
    first->restore_devtools({{"mode", "embedded"}});
    pump(windows, [&] { return fills_panel(panel, inspector); }, "Embed maximized inspector");
    shortcut(first); pump(windows, [&] { return !visible(first); });
    first->restore_devtools({{"mode", "floating"}});
    CHECK(!visible(first) && GetAncestor(inspector, GA_ROOT) == inspector);
    first->devtools(); pump(windows, [&] { return visible(first); });
    CHECK(IsZoomed(inspector));
    ShowWindow(inspector, SW_RESTORE);
    first->restore_devtools({{"mode", "embedded"}});
    pump(windows, [&] { return fills_panel(panel, inspector); });
    for (const auto width : {520, 360, 1000}) {
      SetWindowPos(native, nullptr, 0, 0, width, 500, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      pump(windows, [&] { return fills_panel(panel, inspector); }, "Small embedded viewport");
      embedded_decorations(panel, inspector);
    }
    shortcut(first); pump(windows, [&] { return !visible(first); });
    CHECK(!visible(first) && first->focused() && IsWindow(inspector));
    first->devtools(); pump(windows, [&] { return visible(first); });
    auto reaper = CreateWindowW(L"STATIC", L"DevTools test REAPER root", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
      30, 30, 1100, 750, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(reaper);
    first->prepare_dock();
    SetWindowLongPtrW(native, GWL_STYLE, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN);
    SetParent(native, reaper); MoveWindow(native, 0, 0, 1000, 650, TRUE);
    first->tick();
    CHECK(GetAncestor(inspector, GA_ROOT) == reaper);
    inspector_shortcut(inspector, windows); pump(windows, [&] { return !visible(first); }, "Docked inspector shortcut");
    MoveWindow(native, 0, 0, 900, 600, TRUE);
    CHECK(!IsWindowVisible(panel) && !IsWindowVisible(divider));
    first->devtools(); pump(windows, [&] { return visible(first); });
    first->restore_devtools({{"mode", "floating"}});
    auto floating = GetAncestor(inspector, GA_ROOT);
    CHECK(GetWindow(floating, GW_OWNER) == reaper);
    SetForegroundWindow(reaper); first->tick();
    CHECK(GetForegroundWindow() == reaper && !(GetWindowLongPtrW(floating, GWL_EXSTYLE) & WS_EX_TOPMOST));
    first->restore_floating(); first->tick();
    CHECK(GetWindow(floating, GW_OWNER) == native);
    first->restore_devtools({{"mode", "embedded"}});
    CHECK(GetAncestor(inspector, GA_ROOT) == native && navigations == 1);
    DestroyWindow(reaper);
    const auto before = messages;
    pump(windows, [&] { return messages > before + 3; });
    CHECK(first->diagnostics()["controllerVisible"] == true && navigations == 1);
    first->devtools(); pump(windows, [&] { return visible(first); }); // Existing API remains idempotent.
    inspector_shortcut(inspector, windows); pump(windows, [&] { return !visible(first); }, "Inspector shortcut hide");
    pump(windows, [&] { return first->focused(); });
    CHECK(IsWindow(inspector));
    SetWindowPos(native, nullptr, 0, 0, 920, 650, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    CHECK(!IsWindowVisible(panel) && !IsWindowVisible(divider));
    shortcut(first); pump(windows, [&] { return visible(first); });
    CHECK(inspector_for(first) == inspector); // Hide/show reuses the native session.
    const auto count = browser_windows(inspector);
    for (int i = 0; i < 3; ++i) {
      inspector_shortcut(inspector, windows); pump(windows, [&] { return !visible(first); });
      shortcut(first); pump(windows, [&] { return visible(first); });
      CHECK(inspector_for(first) == inspector && browser_windows(inspector) == count);
    }
    WNDCLASSW probe_class{}; probe_class.hInstance = GetModuleHandleW(nullptr);
    probe_class.lpfnWndProc = shortcut_probe; probe_class.lpszClassName = L"ReaWebAPI.ShortcutProbe";
    CHECK(RegisterClassW(&probe_class));
    int delivered = 0;
    auto probe = CreateWindowW(probe_class.lpszClassName, L"Unmanaged shortcut test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
      0, 0, 300, 200, nullptr, nullptr, probe_class.hInstance, &delivered);
    CHECK(probe);
    inspector_shortcut(probe, windows);
    pump(windows, [&] { return delivered >= 2; }, "Unmanaged shortcut passthrough");
    CHECK(visible(first) && browser_windows(inspector) == count);
    DestroyWindow(probe);
    UnregisterClassW(probe_class.lpszClassName, probe_class.hInstance);
    first->restore_devtools({{"mode", "floating"}});
    PostMessageW(inspector, WM_CLOSE, 0, 0);
    pump(windows, [&] { return !IsWindow(inspector); });
    first->restore_devtools({{"mode", "embedded"}});
    shortcut(first); pump(windows, [&] { return visible(first); });
    auto independent_options = options; independent_options.on_dock_toggle = {}; independent_options.is_docked = {};
    auto second = platform->open(independent_options); windows.push_back(second);
    second->devtools();
    pump(windows, [&] { return visible(second); });
    auto second_inspector = inspector_for(second);
    CHECK(IsChild(static_cast<HWND>(second->native_handle()), second_inspector));
    CHECK(visible(first));
    inspector_shortcut(second_inspector, windows); pump(windows, [&] { return !visible(second); });
    CHECK(visible(first));
    second->devtools(); pump(windows, [&] { return visible(second); });
    CHECK(inspector_for(second) == second_inspector);
    windows.erase(windows.begin() + 1); second.reset();
    pump(windows, [&] { return !IsWindow(second_inspector); });
    if (!visible(first) || !error.empty()) std::cerr << "After closing second: " << first->diagnostics().dump() << " error=" << error << '\n';
    CHECK(visible(first) && error.empty());
    inspector_shortcut(inspector_for(first), windows); pump(windows, [&] { return !visible(first); });
    first->restore_devtools({{"mode", "floating"}, {"widthRatio", ratio}});
    const auto saved = first->devtools_state();
    windows.clear(); first.reset();
    auto reopened = platform->open(options); windows.push_back(reopened);
    reopened->restore_devtools(saved);
    CHECK(!visible(reopened) && reopened->devtools_state() == saved);
    reopened->devtools(); pump(windows, [&] { return visible(reopened); });
    CHECK(reopened->diagnostics()["devtools"]["mode"] == "floating");
    CHECK(GetAncestor(inspector_for(reopened), GA_ROOT) != reopened->native_handle());
    inspector_shortcut(inspector_for(reopened), windows); pump(windows, [&] { return !visible(reopened); });
    std::cout << "WebView2 DevTools: borderless renderer bounds, 1px divider, native floating controls/placement, session reuse, saved preferences, shortcuts, hide/close and multi-window isolation passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
