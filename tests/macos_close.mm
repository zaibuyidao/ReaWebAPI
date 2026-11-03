// Opt-in native teardown acceptance in an isolated REAPER profile.
#include <reaper_plugin.h>
#include "platform/platform.hpp"
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <objc/runtime.h>
#include <fstream>

namespace {
using namespace reaweb;
reaper_plugin_info_t* host;
fs::path resource;
WKWebView* watched = nil;
IMP original_remove = nullptr;
int id = 0, mode = 0, repeat = 0, phase = 0;
bool removed = false, exposed = false;
DWORD started, due;
void tick();
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void log(const std::string& message) { std::ofstream(resource / "close-test.log", std::ios::app) << message << '\n'; }
void observe_remove(NSView* view, SEL selector) {
  if (view == watched) { removed = true; exposed |= view.window.visible; }
  reinterpret_cast<void(*)(::id, SEL)>(original_remove)(view, selector);
}
WKWebView* find_view() {
  for (NSWindow* window in NSApp.windows) {
    std::vector<NSView*> queue{window.contentView};
    while (!queue.empty()) {
      auto candidate = queue.back(); queue.pop_back();
      if ([candidate isKindOfClass:WKWebView.class]) return (WKWebView*)candidate;
      for (NSView* child in candidate.subviews) queue.push_back(child);
    }
  }
  return nil;
}
void finish(const std::string& message) {
  log(message); host->Register("-timer", reinterpret_cast<void*>(tick));
  if (original_remove) method_setImplementation(class_getInstanceMethod(NSView.class, @selector(removeFromSuperview)), original_remove);
  watched = nil;
  PostMessage(host->hwnd_main, WM_CLOSE, 0, 0);
}
void tick() {
  try {
    require(GetTickCount() - started < 120000, "Close acceptance timed out");
    if (GetTickCount() < due || !IsWindowVisible(host->hwnd_main)) return;
    auto open = reinterpret_cast<int(*)(const char*, const char*)>(host->GetFunc("ReaWeb_OpenEx"));
    if (!open) return;
    if (phase == 0) {
      id = open((resource / "Scripts/Close/index.html").c_str(), mode == 1 ? "{}" : mode == 2 ?
        "{\"frameless\":true,\"resizable\":false}" : "{\"frameless\":true}");
      require(id != 0, "Open failed"); phase = 1; due = GetTickCount() + 300; return;
    }
    if (phase == 1) {
      if (!reinterpret_cast<bool(*)(int)>(host->GetFunc("ReaWeb_IsReady"))(id)) return;
      watched = find_view(); if (!watched || !watched.window.visible) return;
      require(bool(watched.window.styleMask & NSWindowStyleMaskTitled) == (mode == 1), "Incorrect title bar style");
      removed = exposed = false;
      if (mode == 2) [watched evaluateJavaScript:@"reaper.window.close()" completionHandler:nil];
      else require(reinterpret_cast<bool(*)(int)>(host->GetFunc("ReaWeb_Close"))(id), "Close failed");
      phase = 2; return;
    }
    if (!removed) return;
    require(!exposed, "WKWebView removed while its native window was visible");
    require(!watched.window, "Closed WebView retained its native window");
    watched = nil;
    log("PASS: hide before WKWebView teardown, repeat=" + std::to_string(repeat) + " mode=" + std::to_string(mode));
    phase = 0; due = GetTickCount() + 100;
    if (++mode == 3) { mode = 0; if (++repeat == 5) finish("PASS: all close cases"); }
  } catch (const std::exception& error) { finish(std::string("FAIL: ") + error.what()); }
}
}
extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t* rec) {
  if (!rec) return 0;
  host = rec;
  resource = fs::u8path(reinterpret_cast<const char*(*)()>(host->GetFunc("GetResourcePath"))());
  if (!fs::exists(resource / "close-test.enabled")) return 0;
  auto page = resource / "Scripts/Close/index.html"; fs::create_directories(page.parent_path());
  std::ofstream(page.parent_path() / "app.json") << R"({"id":"close-acceptance"})";
  std::ofstream(page) << "<!doctype html><title>Close acceptance</title><style>html{background:#182838}</style><input value=Close>";
  original_remove = method_setImplementation(class_getInstanceMethod(NSView.class, @selector(removeFromSuperview)), reinterpret_cast<IMP>(observe_remove));
  started = GetTickCount(); host->Register("timer", reinterpret_cast<void*>(tick)); return 1;
}
