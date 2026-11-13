// Opt-in native keyboard acceptance, controlled through an isolated profile.
#include <reaper_plugin.h>
#include "platform/platform.hpp"
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <fstream>
#include <unistd.h>

@interface WKWebView (IsolationTestProcess)
- (pid_t)_webProcessIdentifier;
@end

namespace {
using namespace reaweb;
reaper_plugin_info_t* host;
fs::path resource;
int window_id = 0, shortcuts = 0, last_command = 0;
bool pending = false;
WKWebView* view;
template<class T> T api(const char* name) { return reinterpret_cast<T>(host->GetFunc(name)); }
NSWindow* main_window() {
  id object = (__bridge id)host->hwnd_main;
  return [object isKindOfClass:NSWindow.class] ? object : [object window];
}
WKWebView* find_view(NSView* root) {
  if ([root isKindOfClass:WKWebView.class]) return (WKWebView*)root;
  for (NSView* child in root.subviews) if (auto found = find_view(child)) return found;
  return nil;
}
void reply(Json value) {
  std::ofstream(resource / "keyboard-reply.tmp") << value.dump();
  fs::rename(resource / "keyboard-reply.tmp", resource / "keyboard-reply.json");
  pending = false;
}
bool command_hook(KbdSectionInfo*, int command, int, int, int, HWND) {
  if (command == 40044 || command == 1016) { ++shortcuts; last_command = command; }
  return false;
}
void tick() {
  if (pending) return;
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
  if (!IsWindowVisible(host->hwnd_main)) return;
  try {
    const auto path = resource / "keyboard-command.json";
    if (!fs::exists(path)) return;
    Json input; { std::ifstream file(path); file >> input; } fs::remove(path);
    const std::string op = input.at("op");
    std::ofstream(resource / "keyboard-stage.txt") << op;
    if (op == "open") {
      window_id = api<int(*)(const char*, const char*)>("ReaWeb_OpenEx")((resource / "Scripts/Keyboard/index.html").c_str(), "{\"hidden\":true}");
      std::ofstream(resource / "keyboard-stage.txt") << "opened " << window_id;
      if (!window_id) throw std::runtime_error(api<const char*(*)()>("ReaWeb_GetLastError")());
      api<bool(*)(int)>("ReaWeb_Show")(window_id);
      reply(window_id);
    } else if (op == "jsfx") {
      api<void(*)(int,bool)>("InsertTrackAtIndex")(0, false);
      auto track = api<void*(*)(void*,int)>("GetTrack")(nullptr, 0);
      int fx = api<int(*)(void*,const char*,bool,int)>("TrackFX_AddByName")(track, "keyboard/probe.jsfx", false, -1);
      if (fx < 0) throw std::runtime_error("Cannot load probe JSFX");
      api<void(*)(void*,int,int)>("TrackFX_Show")(track, fx, input.value("floating", true) ? 3 : 1);
      reply(fx);
    } else if (op == "select") {
      view = nil;
      for (NSWindow* native in NSApp.windows) if (native.visible) {
        auto candidate = find_view(native.contentView);
        if (candidate && candidate.URL && ![candidate.URL.absoluteString isEqualToString:@"about:blank"]) { view = candidate; break; }
      }
      reply(bool(view));
    } else if (op == "eval") {
      if (!view) throw std::runtime_error("No WebView");
      pending = true;
      auto script = [NSString stringWithUTF8String:input.at("script").get<std::string>().c_str()];
      [view evaluateJavaScript:script completionHandler:^(id value, NSError* error) {
        if (error) reply({{"error", error.description.UTF8String}});
        else {
          auto data = [NSJSONSerialization dataWithJSONObject:value ?: NSNull.null options:NSJSONWritingFragmentsAllowed error:nil];
          if (!data) reply(nullptr);
          else reply(Json::parse(std::string(static_cast<const char*>(data.bytes), data.length)));
        }
      }];
    } else if (op == "focus") {
      [NSApp activateIgnoringOtherApps:YES];
      if (input.value("main", false)) [main_window() makeKeyAndOrderFront:nil];
      else {
        [view.window makeKeyAndOrderFront:nil];
        if (input.value("content", false)) [view.window makeFirstResponder:view];
      }
      reply(true);
    } else if (op == "key") {
      if (!CGPreflightPostEventAccess()) throw std::runtime_error("Native keyboard injection permission unavailable");
      auto event = CGEventCreateKeyboardEvent(nullptr, input.at("key").get<int>(), input.at("down").get<bool>());
      CGEventSetFlags(event, input.value("flags", uint64_t(0)));
      CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, input.value("repeat", false));
      CGEventPostToPid(getpid(), event); CFRelease(event);
      reply(true);
    } else if (op == "dock") {
      reply(api<bool(*)(int,bool)>("ReaWeb_SetDocked")(window_id, input.at("value").get<bool>()));
    } else if (op == "state") {
      reply({{"shortcuts", shortcuts}, {"command", last_command}, {"keyWindow", bool(view.window.keyWindow)},
        {"webProcessPid", [view respondsToSelector:@selector(_webProcessIdentifier)] ? [view _webProcessIdentifier] : 0},
        {"responder", view.window.firstResponder ? view.window.firstResponder.className.UTF8String : "none"}});
    } else if (op == "quit") {
      if (window_id) api<bool(*)(int)>("ReaWeb_Close")(window_id);
      reply(true);
      host->Register("-timer", reinterpret_cast<void*>(tick));
      PostMessage(host->hwnd_main, WM_CLOSE, 0, 0);
    } else throw std::runtime_error("Unknown keyboard test operation");
  } catch (const std::exception& error) { reply({{"error", error.what()}}); }
}
}
extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t* rec) {
  if (!rec) return 0;
  host = rec; resource = fs::u8path(api<const char*(*)()>("GetResourcePath")());
  if (!fs::exists(resource / "keyboard-test.enabled")) return 0;
  host->Register("hookcommand2", reinterpret_cast<void*>(command_hook));
  host->Register("timer", reinterpret_cast<void*>(tick));
  std::ofstream(resource / "keyboard-ready") << "ready";
  return 1;
}
