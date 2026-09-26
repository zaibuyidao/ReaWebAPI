#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <chrono>
#include "web/web_resources.hpp"

namespace {
std::vector<std::wstring> opened;
HINSTANCE test_shell_open(HWND, LPCWSTR, LPCWSTR url, LPCWSTR, LPCWSTR, INT) {
  opened.emplace_back(url);
  return reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(opened.back() == L"https://example.com/fail" ? 31 : 33));
}
}
// Exercise production callbacks without launching a browser or mail client.
#define ShellExecuteW test_shell_open
#include "../src/platform/windows/platform_win.cpp"
#undef ShellExecuteW

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
int main() {
  try {
    const auto root = fs::temp_directory_path() / ("reaweb-navigation-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(root);
    const auto entry = root / "index.html";
    std::ofstream(entry) << "<!doctype html><title>Navigation test</title><body>Retained page</body>";
    WebResources resources(root, root / "origin");
    auto platform = make_platform(root / "profile");
    for (const auto& url : {file_uri(entry), resources.entry_url(entry), resources.entry_url(entry) + "?dev=1"}) {
      std::string error;
      std::vector<Json> messages;
      int navigations = 0, reloads = 0;
      WindowOptions options;
      options.entry = entry; options.url = url; options.title = "Navigation regression";
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
      CHECK(error.empty() && navigations == 1);
      auto check = [&](const std::string& script, size_t launches, bool warning = false) {
        const auto count = messages.size(), before = opened.size();
        const std::string report = warning ?
          ";(()=>{const timer=setInterval(()=>{if(warnings.length){clearInterval(timer);report();}},50);})();" :
          ";setTimeout(report,150);";
        window->evaluate("warnings=[];" + script + report);
        pump([&] { return messages.size() > count || !error.empty(); });
        CHECK(error.empty() && !window->closed());
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
      window.reset();
    }
    platform.reset();
    std::cout << "WebView2 navigation: file/HTTP/dev entries, external handlers, local/query/hash, popup rejection and failure recovery passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
