#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <chrono>
#include <httplib.h>
#include "web/web_resources.hpp"
#include "platform/windows/win_context_menu.hpp"

namespace {
std::vector<std::wstring> opened;
Microsoft::WRL::ComPtr<ICoreWebView2> active_view;
HRESULT capture_menu(ICoreWebView2* view, std::function<void(POINT)> show) {
  active_view = view;
  return reaweb::install_window_menu(view, std::move(show));
}
int selected_command = 0;
BOOL test_popup(HMENU menu, UINT, int, int, int, HWND, const RECT*) {
  return !(GetMenuState(menu, selected_command, MF_BYCOMMAND) & MF_GRAYED) ? selected_command : 0;
}
HINSTANCE test_shell_open(HWND, LPCWSTR, LPCWSTR url, LPCWSTR, LPCWSTR, INT) {
  opened.emplace_back(url);
  return reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(opened.back() == L"https://example.com/fail" ? 31 : 33));
}
}
// Exercise production callbacks without launching a browser or mail client.
#define install_window_menu capture_menu
#define TrackPopupMenu test_popup
#define ShellExecuteW test_shell_open
#include "../src/platform/windows/platform_win.cpp"
#undef ShellExecuteW
#undef TrackPopupMenu
#undef install_window_menu

using namespace reaweb;
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed: " #value); } while (false)
void pump(const std::function<bool()>& done) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  do {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (done()) return;
    Sleep(5);
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error("Navigation test timed out");
}
void ctrl_key(HWND owner, WORD key) {
  auto target = GetFocus();
  if (!target || !IsChild(owner, target)) {
    EnumChildWindows(owner, [](HWND child, LPARAM data) -> BOOL {
      wchar_t name[128]{};
      GetClassNameW(child, name, 128);
      if (wcscmp(name, L"Chrome_RenderWidgetHostHWND")) return TRUE;
      *reinterpret_cast<HWND*>(data) = child;
      return FALSE;
    }, reinterpret_cast<LPARAM>(&target));
  }
  CHECK(target && IsChild(owner, target));
  BYTE saved[256]{}, keys[256]{};
  CHECK(GetKeyboardState(saved));
  memcpy(keys, saved, sizeof(keys));
  keys[VK_CONTROL] = 0x80; keys[VK_SHIFT] = keys[VK_MENU] = 0;
  CHECK(SetKeyboardState(keys));
  SendMessageW(target, WM_KEYDOWN, key, 1);
  SendMessageW(target, WM_KEYUP, key, (LPARAM(3) << 30) | 1);
  CHECK(SetKeyboardState(saved));
}
int main(int argc, char** argv) {
  try {
    const auto root = fs::temp_directory_path() / ("reaweb-navigation-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(root);
    const auto entry = root / "index.html";
    std::ofstream(entry) << "<!doctype html><title>Navigation test</title><body>Retained page</body>";
    httplib::Server dev;
    CHECK(dev.set_mount_point("/", root.u8string()));
    const auto port = dev.bind_to_any_port("127.0.0.1"); CHECK(port > 0);
    std::thread serving([&] { dev.listen_after_bind(); });
    struct Stop { httplib::Server& server; std::thread& thread; ~Stop() { server.stop(); thread.join(); } } stop{dev, serving};
    auto resources = std::make_shared<WebResources>(root, "navigation-test");
    const auto nested = root / u8"sub folder 中文";
    fs::create_directories(nested);
    const auto settings = nested / "settings.html";
    std::ofstream(settings) << "<title>Settings</title><body>unique</body>";
    CHECK(html_folder(resources->entry_url(settings) + "#tab", entry, resources.get()) == nested);
    CHECK(html_folder(file_uri(settings), settings, nullptr) == nested);
    CHECK(html_folder("http://localhost/settings.html", entry, nullptr).empty());
    auto platform = make_platform(root / "profile");
    if (argc > 1 && std::string(argv[1]) == "--menu") {
      WindowOptions options;
      options.entry = settings; options.resources = resources; options.url = resources->entry_url(settings);
      options.title = "Page menu smoke test";
      int loaded = 0, reload_requests = 0;
      std::string error;
      options.script = "addEventListener('DOMContentLoaded',()=>chrome.webview.postMessage('ready'))";
      options.on_message = [&](std::string message) { if (message == "ready") ++loaded; };
      options.on_reload = [&] { ++reload_requests; return false; };
      options.on_error = [&](std::string message) { error = message; };
      auto window = platform->open(std::move(options));
      pump([&] { return loaded || !error.empty(); });
      CHECK(error.empty());
      auto hwnd = static_cast<HWND>(window->native_handle());
      window->focus(); SetForegroundWindow(hwnd);
      pump([&] { window->tick(); return window->visible(); });
      selected_command = 5;
      SendMessageW(hwnd, WM_CONTEXTMENU, 0, -1);
      CHECK(opened.size() == 1 && fs::equivalent(fs::path(opened.back()), nested));
      Microsoft::WRL::ComPtr<ICoreWebView2_28> modern;
      Microsoft::WRL::ComPtr<ICoreWebView2Find> find;
      CHECK(SUCCEEDED(active_view.As(&modern)) && SUCCEEDED(modern->get_Find(&find)));
      window->focus();
      ctrl_key(hwnd, 'R');
      pump([&] { return loaded == 2 || !error.empty(); });
      CHECK(error.empty() && reload_requests == 2);
      window->focus();
      ctrl_key(hwnd, 'F');
      selected_command = 7;
      SendMessageW(hwnd, WM_CONTEXTMENU, 0, -1);
      CHECK(error.empty());
      Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
      Microsoft::WRL::ComPtr<ICoreWebView2Environment15> factory;
      Microsoft::WRL::ComPtr<ICoreWebView2FindOptions> query;
      CHECK(SUCCEEDED(modern->get_Environment(&environment)) && SUCCEEDED(environment.As(&factory)));
      CHECK(SUCCEEDED(factory->CreateFindOptions(&query)) && SUCCEEDED(query->put_FindTerm(L"unique")));
      bool searched = false;
      CHECK(SUCCEEDED(find->Start(query.Get(), Microsoft::WRL::Callback<ICoreWebView2FindStartCompletedHandler>(
        [&](HRESULT hr) -> HRESULT { searched = SUCCEEDED(hr); return S_OK; }).Get())));
      pump([&] { INT32 matches = 0; return searched && SUCCEEDED(find->get_MatchCount(&matches)) && matches == 1; });
      CHECK(SUCCEEDED(find->Stop()));
      active_view.Reset();
      std::cout << "WebView2: nested HTML folder, Find command and Ctrl+R reload passed\n";
      return 0;
    }
    for (const auto& url : {file_uri(entry), resources->entry_url(entry), "http://127.0.0.1:" + std::to_string(port) + "/index.html"}) {
      std::string error;
      std::vector<Json> messages;
      int navigations = 0, reloads = 0;
      WindowOptions options;
      options.entry = entry; options.url = url; options.title = "Navigation regression";
      if (url.rfind("reaweb:", 0) == 0) options.resources = resources;
      options.script = R"(window.retained=42; window.warnings=[];
        console.warn=(text)=>warnings.push(text);
        window.report=()=>chrome.webview.postMessage(JSON.stringify({url:location.href,retained,warnings}));
        addEventListener('DOMContentLoaded',report);)";
      options.on_message = [&](std::string text) { messages.push_back(Json::parse(text)); };
      options.on_error = [&](std::string text) { error = text; };
      options.on_navigation = [&] { ++navigations; };
      options.on_reload = [&] { ++reloads; return false; };
      auto window = platform->open(std::move(options));
      pump([&] { return !messages.empty() || !error.empty(); });
      if (!error.empty()) throw std::runtime_error(error);
      CHECK(navigations == 1);
      auto check = [&](const std::string& script, size_t launches, bool warning = false) {
        const auto count = messages.size(), before = opened.size();
        const std::string report = warning ?
          ";(()=>{const timer=setInterval(()=>{if(warnings.length){clearInterval(timer);report();}},50);})();" :
          ";setTimeout(report,150);";
        window->evaluate("warnings=[];" + script + report);
        pump([&] { return messages.size() > count || !error.empty(); });
        if (!error.empty()) throw std::runtime_error(script + ": " + error);
        CHECK(!window->closed());
        CHECK(messages.back()["retained"] == 42 && same_document(messages.back()["url"], url));
        CHECK(navigations == 1 && reloads == 1 && opened.size() == before + launches);
        if (messages.back()["warnings"].empty() == warning)
          throw std::runtime_error(script + ": " + messages.back().dump());
      };
      check("location.hash='section'", 0);
      check("location.href='https://example.com/fail'", 1, true);
      check("const a=document.createElement('a');a.href='https://example.com/link';a.click()", 1);
      CHECK(opened.back() == L"https://example.com/link");
      check("location.href='http://example.com/href'", 1);
      check("location.assign('https://example.com/assign')", 1);
      check("location.assign('mailto:test@example.com?subject=Hi')", 1);
      check("location.assign('other.html')", 0, true);
      CHECK(messages.back()["warnings"][0].get<std::string>().find("reaper.window.open(path)") != std::string::npos);
      check("location.search='?changed=1'", 0, true);
      check("window.open('https://example.com/popup')", 0);
      check("const b=document.createElement('a');b.href='https://example.com/blank';b.target='_blank';b.click()", 0);
      check("location.href='custom:unsupported'", 0, true);
      active_view.Reset();
      window.reset();
    }
    platform.reset();
    std::cout << "WebView2 navigation: file/virtual/HTTP dev entries, external handlers, local/query/hash, popup rejection and failure recovery passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
