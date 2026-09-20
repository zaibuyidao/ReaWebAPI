#include "platform/platform.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <chrono>
using namespace reaweb;
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed: " #value); } while (false)
void pump(const std::vector<std::shared_ptr<Window>>& windows, const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  do {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    for (auto& window : windows) window->tick();
    if (done()) return;
    Sleep(5);
  } while (std::chrono::steady_clock::now() < deadline);
  for (auto& window : windows) std::cerr << window->diagnostics().dump() << '\n';
  throw std::runtime_error("DevTools test timed out");
}
void shortcut(const std::shared_ptr<Window>& window) {
  // Exercise the native shortcut without injecting input into the user's desktop.
  window->focus();
  auto root = static_cast<HWND>(window->native_handle());
  auto target = GetFocus();
  CHECK(target && target != root && IsChild(root, target));
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
    options.entry = entry; options.title = "ReaWebAPI DevTools test";
    options.script = "window.token='retained';setInterval(()=>chrome.webview.postMessage(window.token),50);console.log('retained console entry');";
    options.on_message = [&](std::string text) { CHECK(text == "retained"); ++messages; };
    options.on_error = [&](std::string message) { error = message; };
    options.on_navigation = [&] { ++navigations; };
    auto first = platform->open(options);
    std::vector<std::shared_ptr<Window>> windows{first};
    auto visible = [](const auto& window) { return window->diagnostics()["devtools"]["visible"].template get<bool>(); };
    pump(windows, [&] { return messages > 0; });
    shortcut(first); pump(windows, [&] { return visible(first); });
    auto inspector = inspector_for(first);
    CHECK(GetPropW(inspector, L"ReaWebAPI.DevTools.Owner"));
    CHECK(GetWindow(inspector, GW_OWNER) == first->native_handle());
    const auto before = messages;
    pump(windows, [&] { return messages > before + 3; });
    CHECK(first->diagnostics()["controllerVisible"] == true && navigations == 1);
    first->devtools(); pump(windows, [&] { return visible(first); }); // Existing API remains idempotent.
    shortcut(first); pump(windows, [&] { return !visible(first); });
    CHECK(IsWindow(inspector));
    shortcut(first); pump(windows, [&] { return visible(first); });
    CHECK(inspector_for(first) == inspector); // Hide/show reuses the native session.
    PostMessageW(inspector, WM_CLOSE, 0, 0);
    pump(windows, [&] { return !IsWindow(inspector); });
    shortcut(first); pump(windows, [&] { return visible(first); });
    auto second = platform->open(options); windows.push_back(second);
    second->devtools();
    pump(windows, [&] { return visible(second); });
    auto second_inspector = inspector_for(second);
    CHECK(GetWindow(second_inspector, GW_OWNER) == second->native_handle());
    CHECK(visible(first));
    windows.erase(windows.begin() + 1); second.reset();
    pump(windows, [&] { return !IsWindow(second_inspector); });
    if (!visible(first) || !error.empty()) std::cerr << "After closing second: " << first->diagnostics().dump() << " error=" << error << '\n';
    CHECK(visible(first) && error.empty());
    std::cout << "WebView2 DevTools: native shortcut messages, idempotent open, session reuse, close/reopen, live page and multi-window ownership passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
