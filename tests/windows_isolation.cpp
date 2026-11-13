#include "../src/platform/windows/platform_win.cpp"
#include "bridge_script.hpp"
#include <fstream>
#include <iostream>
using namespace reaweb;
int main() {
  try {
    const auto root = fs::current_path() / ".cache" / ("isolation-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(root);
    std::ofstream(root / "index.html") << "<!doctype html><title>Isolation</title>";
    auto resources = std::make_shared<WebResources>(root, "isolation-test", true);
    auto platform = make_platform(root / "profile");
    WindowOptions options;
    options.entry = root / "index.html"; options.url = resources->entry_url(options.entry);
    options.resources = resources; options.hidden = true;
    options.script = R"(addEventListener('DOMContentLoaded',()=>chrome.webview.postMessage(JSON.stringify({
      isolated:crossOriginIsolated,secure:isSecureContext,sab:typeof SharedArrayBuffer,origin:location.origin
    })));)";
    std::string report, error;
    options.on_message = [&](std::string text) { report = text; };
    options.on_error = [&](std::string text) { error = text; };
    auto window = platform->open(options);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (report.empty() && error.empty() && std::chrono::steady_clock::now() < deadline) {
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
      Sleep(5);
    }
    if (report.empty() || !error.empty() || !Json::parse(report).value("isolated", false))
      throw std::runtime_error("Isolation failed: " + report + error);
    std::cout << report << '\n';
    window.reset();
    // Deliberately omit the required headers to exercise the document-start guard.
    std::ofstream(root / "index.html") << "<!doctype html><script>chrome.webview.postMessage('APP_EXECUTED')</script>";
    options.resources = std::make_shared<WebResources>(root, "isolation-test");
    options.script = "(() => { const reawebIsolationRequired = true;" + bridge_script + "})();";
    bool app_executed = false;
    report.clear(); error.clear();
    options.on_message = [&](std::string text) {
      if (text == "APP_EXECUTED") app_executed = true;
      else if (text.rfind("__reawebBrowser:", 0) == 0) report = text.substr(16);
    };
    window = platform->open(options);
    const auto fail_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < fail_deadline) {
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
      Sleep(5);
    }
    if (app_executed || report.empty() || Json::parse(report).value("error", "") != "CROSS_ORIGIN_ISOLATION_FAILED")
      throw std::runtime_error("Required document was not stopped before App execution: " + report);
    std::cout << "PASS: required policy failure reported before handshake and App script execution\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

