// Opt-in native overlay acceptance in an isolated REAPER profile.
#include <reaper_plugin.h>
#include "platform/platform.hpp"
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <objc/runtime.h>
#include <fstream>
#include <unistd.h>

namespace {
using namespace reaweb;
reaper_plugin_info_t* host;
fs::path resource;
std::vector<int> ids;
WKWebView* view;
NSWindow* floating;
IMP original_dispatch;
Method dispatch_method;
bool pending = false;
NSPoint saved_cursor;
Json events = Json::array();
template<class T> T api(const char* name) { return reinterpret_cast<T>(host->GetFunc(name)); }
NSWindow* main_window() { return [(__bridge NSView*)host->hwnd_main window]; }
void dispatch(id app, SEL selector, NSEvent* event) {
  if (event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeLeftMouseUp || event.type == NSEventTypeLeftMouseDragged ||
      event.type == NSEventTypeRightMouseDown || event.type == NSEventTypeRightMouseUp || event.type == NSEventTypeScrollWheel)
    events.push_back({{"window", event.window == view.window ? "overlay" : event.window == main_window() ? "main" : "other"}, {"type", event.type}});
  reinterpret_cast<void(*)(id,SEL,NSEvent*)>(original_dispatch)(app, selector, event);
}
WKWebView* find_view(NSView* root) {
  if ([root isKindOfClass:WKWebView.class]) return (WKWebView*)root;
  for (NSView* child in root.subviews) if (auto found = find_view(child)) return found;
  return nil;
}
void reply(Json value) {
  std::ofstream(resource / "overlay-reply.tmp") << value.dump();
  fs::rename(resource / "overlay-reply.tmp", resource / "overlay-reply.json"); pending = false;
}
void tick() {
  if (pending) return;
  for (NSWindow* window in NSApp.windows) if (window.visible && [window.title hasPrefix:@"About REAPER"]) {
    std::vector<NSView*> queue{window.contentView};
    while (!queue.empty()) {
      auto child = queue.back(); queue.pop_back();
      if ([child isKindOfClass:NSButton.class] && [[(NSButton*)child title] containsString:@"Still Evaluating"] && [(NSButton*)child isEnabled])
        [(NSButton*)child performClick:nil];
      for (NSView* next in child.subviews) queue.push_back(next);
    }
    return;
  }
  if (NSApp.modalWindow) {
    std::vector<NSView*> queue{NSApp.modalWindow.contentView};
    while (!queue.empty()) {
      auto child = queue.back(); queue.pop_back();
      if ([child isKindOfClass:NSButton.class] && [[(NSButton*)child title] isEqualToString:@"No"])
        [(NSButton*)child performClick:nil];
      for (NSView* next in child.subviews) queue.push_back(next);
    }
    return;
  }
  try {
    const auto path = resource / "overlay-command.json";
    if (!fs::exists(path)) return;
    Json input; { std::ifstream file(path); file >> input; } fs::remove(path);
    const std::string op = input.at("op");
    if (op == "open") {
      auto options = input.at("options").dump();
      auto id = api<int(*)(const char*,const char*)>("ReaWeb_OpenEx")((resource / "Scripts/Overlay/index.html").c_str(), options.c_str());
      if (!id) throw std::runtime_error(api<const char*(*)()>("ReaWeb_GetLastError")());
      ids.push_back(id); reply(id);
    } else if (op == "select") {
      view = nil;
      for (NSWindow* native in NSApp.windows) {
        auto candidate = find_view(native.contentView);
        if (candidate && candidate.URL && ![candidate.URL.absoluteString isEqualToString:@"about:blank"]) { view = candidate; break; }
      }
      reply(bool(view));
    } else if (op == "eval") {
      if (!view) throw std::runtime_error("No WebView");
      pending = true;
      auto script = [NSString stringWithUTF8String:input.at("script").get<std::string>().c_str()];
      [view callAsyncJavaScript:script arguments:@{} inFrame:nil inContentWorld:WKContentWorld.pageWorld completionHandler:^(id value, NSError* error) {
        if (error) reply({{"error", error.description.UTF8String}});
        else {
          auto data = [NSJSONSerialization dataWithJSONObject:value ?: NSNull.null options:NSJSONWritingFragmentsAllowed error:nil];
          reply(data ? Json::parse(std::string(static_cast<const char*>(data.bytes), data.length)) : Json());
        }
      }];
    } else if (op == "place") {
      [main_window() setFrame:NSMakeRect(30, 50, 1000, 750) display:YES];
      [view.window setFrame:NSMakeRect(180, 250, 600, 420) display:YES]; reply(true);
    } else if (op == "zoom") { view.pageZoom = input.at("value").get<double>(); reply(true);
    } else if (op == "focus") {
      [NSApp activateIgnoringOtherApps:YES];
      const std::string target = input.value("target", "overlay");
      if (target == "overlay") api<bool(*)(int)>("ReaWeb_Focus")(ids.front());
      else [target == "main" ? main_window() : floating makeKeyAndOrderFront:nil];
      reply(true);
    } else if (op == "deactivate") {
      auto finder = [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.finder"].firstObject;
      reply(bool([finder activateWithOptions:NSApplicationActivateIgnoringOtherApps]));
    } else if (op == "floating") {
      if (input.value("show", true)) {
        Class kind = input.value("panel", false) ? NSPanel.class : NSWindow.class;
        floating = [[kind alloc] initWithContentRect:NSMakeRect(850, 500, 200, 200) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        floating.releasedWhenClosed = NO; floating.title = @"Overlay acceptance float";
        floating.level = main_window().level; [floating orderFront:nil];
      } else { [floating close]; floating = nil; }
      reply(true);
    } else if (op == "minimize") {
      if (input.at("value").get<bool>()) [main_window() miniaturize:nil]; else [main_window() deminiaturize:nil];
      reply(true);
    } else if (op == "point") {
      if (!CGPreflightPostEventAccess()) throw std::runtime_error("Native input permission unavailable");
      const double x = input.at("x").get<double>() * view.pageZoom, y = input.at("y").get<double>() * view.pageZoom;
      auto point = [view.window convertPointToScreen:[view convertPoint:NSMakePoint(x, view.flipped ? y : view.bounds.size.height-y) toView:nil]];
      const auto global = CGPointMake(point.x, CGDisplayBounds(CGMainDisplayID()).size.height-point.y);
      reply(Json::array({global.x, global.y}));
    } else if (op == "key") {
      auto event = CGEventCreateKeyboardEvent(nullptr, input.at("key").get<int>(), input.at("down").get<bool>());
      CGEventPostToPid(getpid(), event); CFRelease(event); reply(true);
    } else if (op == "state") {
      auto ordered = [NSMutableArray<NSWindow*> array];
      Json windows = Json::array();
      Json stack = Json::array();
      auto native = (__bridge_transfer NSArray*)CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
      for (NSDictionary* item in native) if ([item[(id)kCGWindowOwnerPID] intValue] == getpid()) {
        const auto number = [item[(id)kCGWindowNumber] intValue]; stack.push_back(number);
        if (auto window = [NSApp windowWithWindowNumber:number]) [ordered addObject:window];
      }
      for (NSWindow* window in ordered) windows.push_back({{"title", window.title.UTF8String ?: ""}, {"number", window.windowNumber}, {"class", window.className.UTF8String}, {"level", window.level}, {"webview", bool(find_view(window.contentView))}});
      reply({{"main", [ordered indexOfObject:main_window()]}, {"overlay", [ordered indexOfObject:view.window]},
        {"floating", floating ? [ordered indexOfObject:floating] : NSNotFound}, {"visible", bool(view.window.visible)},
        {"key", bool(view.window.keyWindow)}, {"ignores", bool(view.window.ignoresMouseEvents)}, {"events", events},
        {"accessibility", bool(AXIsProcessTrusted())}, {"active", bool(NSApp.active)}, {"windows", windows}, {"stack", stack}, {"overlayNumber", view.window.windowNumber}});
      events = Json::array();
    } else if (op == "quit") {
      for (auto id : ids) api<bool(*)(int)>("ReaWeb_Close")(id);
      if (original_dispatch) method_setImplementation(dispatch_method, original_dispatch);
      [floating close]; floating = nil;
      CGWarpMouseCursorPosition(CGPointMake(saved_cursor.x, CGDisplayBounds(CGMainDisplayID()).size.height-saved_cursor.y));
      reply(true); host->Register("-timer", reinterpret_cast<void*>(tick)); PostMessage(host->hwnd_main, WM_CLOSE, 0, 0);
    } else throw std::runtime_error("Unknown overlay test operation");
  } catch (const std::exception& error) { reply({{"error", error.what()}}); }
}
}
extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t* rec) {
  if (!rec) return 0;
  host = rec; resource = fs::u8path(api<const char*(*)()>("GetResourcePath")());
  if (!fs::exists(resource / "overlay-test.enabled")) return 0;
  saved_cursor = NSEvent.mouseLocation;
  dispatch_method = class_getInstanceMethod(NSApp.class, @selector(sendEvent:));
  original_dispatch = method_setImplementation(dispatch_method, reinterpret_cast<IMP>(dispatch));
  host->Register("timer", reinterpret_cast<void*>(tick)); std::ofstream(resource / "overlay-ready") << "ready"; return 1;
}
