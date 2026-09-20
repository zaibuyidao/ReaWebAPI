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
    auto& search = *reinterpret_cast<Search*>(data);
    if (GetPropW(hwnd, L"ReaWebAPI.DevTools.Owner") && GetWindow(hwnd, GW_OWNER) == search.owner) search.result = hwnd;
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.result;
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
  const auto thread = GetCurrentThreadId();
  const auto foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
  const bool attached = foreground != thread && AttachThreadInput(thread, foreground, TRUE);
  SetForegroundWindow(inspector);
  if (attached) AttachThreadInput(thread, foreground, FALSE);
  pump(windows, [&] { return GetForegroundWindow() == inspector; }, "Inspector focus");
  if (GetWindowThreadProcessId(inspector, nullptr) == thread) SetFocus(inspector);
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
  CHECK(GetForegroundWindow() == inspector);
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
int main() {
  SetProcessDPIAware();
  try {
    auto path = fs::current_path() / ("devtools-test-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(path);
    auto entry = path / "index.html";
    std::ofstream(entry) << "<!doctype html><h1>DevTools test</h1>";
    auto platform = make_platform(path / "profile");
    int messages = 0, navigations = 0;
    std::string error;
    WindowOptions options;
    bool docked = false;
    options.on_dock_toggle = [&] { docked = !docked; };
    options.is_docked = [&] { return docked; };
    options.entry = entry; options.title = "ReaWebAPI DevTools test";
    options.script = "window.token='retained';setInterval(()=>chrome.webview.postMessage(window.token),50);console.log('retained console entry');";
    options.on_message = [&](std::string text) { CHECK(text == "retained"); ++messages; };
    options.on_error = [&](std::string message) { error = message; };
    options.on_navigation = [&] { ++navigations; };
    auto first = platform->open(options);
    std::vector<std::shared_ptr<Window>> windows{first};
    auto visible = [](const auto& window) { return window->diagnostics()["devtools"]["visible"].template get<bool>(); };
    pump(windows, [&] { return messages > 0; });
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
    shortcut(first); pump(windows, [&] { return visible(first); });
    auto inspector = inspector_for(first);
    CHECK(GetPropW(inspector, L"ReaWebAPI.DevTools.Owner"));
    CHECK(GetWindow(inspector, GW_OWNER) == first->native_handle());
    const auto before = messages;
    pump(windows, [&] { return messages > before + 3; });
    CHECK(first->diagnostics()["controllerVisible"] == true && navigations == 1);
    first->devtools(); pump(windows, [&] { return visible(first); }); // Existing API remains idempotent.
    inspector_shortcut(inspector, windows); pump(windows, [&] { return !visible(first); }, "Inspector shortcut hide");
    pump(windows, [&] { return first->focused(); });
    CHECK(IsWindow(inspector));
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
    PostMessageW(inspector, WM_CLOSE, 0, 0);
    pump(windows, [&] { return !IsWindow(inspector); });
    shortcut(first); pump(windows, [&] { return visible(first); });
    auto second = platform->open(options); windows.push_back(second);
    second->devtools();
    pump(windows, [&] { return visible(second); });
    auto second_inspector = inspector_for(second);
    CHECK(GetWindow(second_inspector, GW_OWNER) == second->native_handle());
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
    windows.clear(); first.reset();
    auto reopened = platform->open(options); windows.push_back(reopened);
    reopened->devtools(); pump(windows, [&] { return visible(reopened); });
    inspector_shortcut(inspector_for(reopened), windows); pump(windows, [&] { return !visible(reopened); });
    std::cout << "WebView2 DevTools: page/inspector shortcuts, repeats, cancelled open, session reuse, close/reopen, live page and multi-window isolation passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
