#include "platform/macos/mac_devtools.hpp"
#include <iostream>

using namespace reaweb;
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed at " + std::to_string(__LINE__) + ": " #value); } while (false)

@interface UnavailableInspectorView : WKWebView
@end
@implementation UnavailableInspectorView
- (BOOL)respondsToSelector:(SEL)selector {
  return selector == @selector(_inspector) ? NO : [super respondsToSelector:selector];
}
@end

std::function<void()> tick;
void pump(const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  do {
    auto event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
      inMode:NSDefaultRunLoopMode dequeue:YES];
    if (event) [NSApp sendEvent:event];
    if (tick) tick();
    if (done()) return;
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error("macOS DevTools test timed out");
}
void settle() {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  pump([&] { return std::chrono::steady_clock::now() >= until; });
}
id evaluate(WKWebView* view, NSString* script) {
  struct Result { bool done = false; id value = nil; NSError* error = nil; };
  auto result = std::make_shared<Result>();
  [view evaluateJavaScript:script completionHandler:^(id value, NSError* error) {
    result->value = value; result->error = error; result->done = true;
  }];
  pump([&] { return result->done; });
  if (result->error) throw std::runtime_error(std::string(script.UTF8String) + ": " + result->error.description.UTF8String);
  return result->value;
}
void shortcut(NSWindow* window, NSEventModifierFlags modifiers, bool repeat = false) {
  auto event = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:modifiers
    timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber context:nil
    characters:@"i" charactersIgnoringModifiers:@"i" isARepeat:repeat keyCode:34];
  [NSApp sendEvent:event];
}
NSMenu* menu(MacDevTools& tools, NSString* first, NSString* second) {
  auto result = [NSMenu new];
  CHECK(tools.insert_menu(result, 0) == 2);
  [result update];
  CHECK([[result itemAtIndex:0].title isEqualToString:first]);
  CHECK([[result itemAtIndex:1].title isEqualToString:second]);
  return result;
}
void snapshot(WKWebView* view, NSString* path) {
  auto done = std::make_shared<bool>(false);
  [view takeSnapshotWithConfiguration:nil completionHandler:^(NSImage* image, NSError* error) {
    if (!error && image) {
      auto rep = [NSBitmapImageRep imageRepWithData:image.TIFFRepresentation];
      [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
    }
    *done = true;
  }];
  pump([&] { return *done; });
}
int main(int argc, char** argv) {
  @autoreleasepool {
    try {
      [NSApplication sharedApplication];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
      [NSApp finishLaunching];
      [NSApp activateIgnoringOtherApps:YES];
      auto window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, 552, 760)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
      window.releasedWhenClosed = NO;
      auto config = [WKWebViewConfiguration new];
      config.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
      auto view = [[WKWebView alloc] initWithFrame:window.contentView.bounds configuration:config];
      view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
      [window.contentView addSubview:view];
      [window makeKeyAndOrderFront:nil];
      [window makeFirstResponder:view];
      [view loadHTMLString:@"<!doctype html><h1>Native Web Inspector</h1><input id='state' value='retained'><script>window.token='retained'</script>" baseURL:nil];
      pump([&] { return !view.loading && [evaluate(view, @"document.readyState") isEqual:@"complete"]; });
      {
        MacDevTools tools(view, [&] { [view.window makeKeyAndOrderFront:nil]; [view.window makeFirstResponder:view]; });
        tick = [&] { tools.tick(); };
        CHECK(view.inspectable && !tools.visible());
        CHECK(tools.diagnostics()["nativeToggleSupported"] == true && tools.diagnostics()["embeddedSupported"] == true);
        tools.restore({{"mode", "embedded"}, {"widthRatio", 0.5}});
        [menu(tools, @"Open DevTools", @"Float DevTools") performActionForItemAtIndex:1];
        CHECK(!tools.visible() && tools.state()["mode"] == "floating");
        [menu(tools, @"Open DevTools", @"Embed DevTools") performActionForItemAtIndex:1];
        CHECK(!tools.visible() && tools.state()["mode"] == "embedded");
        id<ReaWebInspectorSPI> inspector = [(id<ReaWebInspectableSPI>)view _inspector];
        tools.open(); tools.hide();
        pump([&] { return [inspector isConnected] && !tools.visible(); }); settle();
        CHECK(!tools.visible() && NSEqualRects(view.frame, view.superview.bounds));
        tools.open();
        const auto embedded = [&] {
          auto front = [inspector extensionHostWebView];
          return tools.visible() && front.superview == view.superview && front.window == window &&
            std::abs(front.frame.origin.x - NSMaxX(view.frame)) < 1 && front.frame.origin.x > 0 &&
            std::abs(front.frame.size.height - view.superview.bounds.size.height) < 1;
        };
        pump(embedded); settle();
        auto front = [inspector extensionHostWebView];
        CHECK(std::abs(front.frame.size.width - 276) < 1);
        CHECK(tools.diagnostics()["mode"] == "embedded" && !tools.diagnostics().contains("fallbackReason"));
        evaluate(front, @"WI.showConsoleTab(); globalThis.reawebTestToken = 'inspector retained'; 'ok'");
        evaluate(view, @"setTimeout(() => console.log('ReaWeb retained console marker'), 50); 'ok'"); settle();
        // SSH test windows can be occluded, leaving Console messages queued for rendering.
        evaluate(front, @"WI.consoleLogViewController.renderPendingMessages(); 'ok'");
        CHECK([evaluate(front, @"document.body.innerText.includes('ReaWeb retained console marker')") boolValue]);
        if (argc > 1) snapshot(front, [[NSString stringWithUTF8String:argv[1]] stringByAppendingPathComponent:@"embedded-inspector.png"]);
        auto check_session = [&] {
          CHECK([inspector extensionHostWebView] == front && [inspector isConnected]);
          CHECK([evaluate(front, @"reawebTestToken === 'inspector retained' && WI.tabBrowser.selectedTabContentView instanceof WI.ConsoleTabContentView") boolValue]);
          evaluate(front, @"WI.consoleLogViewController.renderPendingMessages(); 'ok'");
          CHECK([evaluate(front, @"document.body.innerText.includes('ReaWeb retained console marker')") boolValue]);
          CHECK([evaluate(view, @"window.token === 'retained' && document.querySelector('#state').value === 'retained'") boolValue]);
        };
        [menu(tools, @"Hide DevTools", @"Float DevTools") performActionForItemAtIndex:1];
        pump([&] { return front.window && front.window != window && front.window.visible; }); settle();
        CHECK(tools.diagnostics()["mode"] == "floating" && tools.state()["mode"] == "floating");
        CHECK((front.window.styleMask & (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)) == (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable));
        CHECK([NSStringFromClass(front.window.class) containsString:@"Inspector"]);
        auto floating_window = front.window;
        CHECK(NSEqualRects(view.frame, view.superview.bounds)); check_session();
        if (argc > 1) snapshot(front, [[NSString stringWithUTF8String:argv[1]] stringByAppendingPathComponent:@"floating-inspector.png"]);
        const auto keys = NSEventModifierFlagOption | NSEventModifierFlagCommand;
        [front.window makeFirstResponder:front];
        shortcut(front.window, keys, true); CHECK(tools.visible());
        shortcut(front.window, keys); settle(); CHECK(!tools.visible() && [inspector isConnected]);
        [menu(tools, @"Open DevTools", @"Embed DevTools") performActionForItemAtIndex:1];
        CHECK(!tools.visible());
        shortcut(window, keys); pump(embedded); settle(); check_session();
        tools.open(); settle(); check_session();
        shortcut(window, NSEventModifierFlagControl | NSEventModifierFlagShift); CHECK(tools.visible());
        [window setContentSize:NSMakeSize(1400, 800)]; settle();
        pump([&] { return std::abs(front.frame.size.width - 700) < 1; }); check_session();
        evaluate(front, @"InspectorFrontendHost.setAttachedWindowWidth(650)"); settle();
        CHECK(std::abs(tools.state()["widthRatio"].get<double>() - 650.0 / 1400) < 0.001);
        evaluate(front, @"InspectorFrontendHost.requestSetDockSide('undocked')"); settle();
        CHECK(tools.state()["mode"] == "floating" && tools.menu_state().floating);
        evaluate(front, @"InspectorFrontendHost.requestSetDockSide('right')"); pump(embedded); settle();
        CHECK(tools.state()["mode"] == "embedded"); check_session();
        tools.perform(DevToolsAction::Float); tools.perform(DevToolsAction::Embed); tools.hide(); settle();
        CHECK(!tools.visible() && [inspector isConnected]);
        tools.open(); pump(embedded); settle(); check_session();
        for (auto size : {NSMakeSize(552, 600), NSMakeSize(440, 300), NSMakeSize(320, 220)}) {
          [window setContentSize:size]; settle(); pump(embedded);
          CHECK(tools.diagnostics()["embeddedSupported"] == true && tools.state()["mode"] == "embedded");
          CHECK(std::abs(front.frame.size.width / size.width - tools.state()["widthRatio"].get<double>()) < 0.005);
          CHECK(view.frame.size.width > 0 && front.frame.size.width > 0);
          check_session();
          tools.perform(DevToolsAction::Float); settle();
          CHECK(front.window == floating_window && front.window.visible);
          CHECK([menu(tools, @"Hide DevTools", @"Embed DevTools") itemAtIndex:1].enabled);
          [menu(tools, @"Hide DevTools", @"Embed DevTools") performActionForItemAtIndex:1];
          settle(); pump(embedded); check_session();
          tools.hide(); settle(); CHECK(NSEqualRects(view.frame, view.superview.bounds));
          tools.open(); settle(); pump(embedded); check_session();
        }
        [window setContentSize:NSMakeSize(1200, 760)]; pump(embedded); settle(); check_session();
        tools.perform(DevToolsAction::Float);
        pump([&] { return front.window != window && front.window.visible; }); settle();
        [front.window performClose:nil]; pump([&] { return !tools.visible() && ![inspector isConnected]; });
        menu(tools, @"Open DevTools", @"Embed DevTools");
        tools.open(); pump([&] { return tools.visible(); }); settle();
        CHECK(tools.diagnostics()["mode"] == "floating");
        front = [inspector extensionHostWebView];
        evaluate(front, @"WI.showElementsTab(); globalThis.reawebTestToken = 'elements retained'; 'ok'");
        tools.perform(DevToolsAction::Embed); pump(embedded); settle();
        auto second = [[NSWindow alloc] initWithContentRect:NSMakeRect(150, 150, 1100, 700)
          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
        second.releasedWhenClosed = NO;
        auto holder = window.contentView;
        window.contentView = [NSView new]; second.contentView = holder;
        [second makeKeyAndOrderFront:nil]; settle();
        CHECK(front.window == second && view.window == second && tools.visible());
        tools.hide(); tools.open();
        pump([&] { return tools.visible() && front.window == second; }); settle();
        CHECK([evaluate(front, @"reawebTestToken === 'elements retained' && WI.tabBrowser.selectedTabContentView instanceof WI.ElementsTabContentView") boolValue]);
        second.contentView = [NSView new]; window.contentView = holder;
        [window makeKeyAndOrderFront:nil]; settle();
        auto other = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 1100, 700) configuration:config];
        [second.contentView addSubview:other];
        [other loadHTMLString:@"<h1>Second independent Inspector</h1>" baseURL:nil];
        pump([&] { return !other.loading; });
        id<ReaWebInspectorSPI> other_inspector;
        {
          MacDevTools other_tools(other, [&] { [second makeFirstResponder:other]; });
          tick = [&] { tools.tick(); other_tools.tick(); };
          other_inspector = [(id<ReaWebInspectableSPI>)other _inspector];
          other_tools.open(); pump([&] { return other_tools.visible(); }); settle();
          CHECK(other_tools.visible() && tools.visible());
          CHECK([other_inspector extensionHostWebView] != front);
          [second makeFirstResponder:other]; shortcut(second, keys); settle();
          CHECK(!other_tools.visible() && tools.visible());
          other_tools.open(); pump([&] { return other_tools.visible(); });
          other_tools.perform(DevToolsAction::Float); other_tools.tick();
          tick = [&] { tools.tick(); };
        }
        settle(); CHECK(![other_inspector isVisible]);
        [second close];
        CHECK([evaluate(front, @"reawebTestToken === 'elements retained' && WI.tabBrowser.selectedTabContentView instanceof WI.ElementsTabContentView") boolValue]);
        tick = {};
      }
      {
        MacDevTools tools(view, [] {});
        id<ReaWebInspectorSPI> inspector = [(id<ReaWebInspectableSPI>)view _inspector];
        [inspector show];
        pump([&] { return [inspector isConnected] && [inspector extensionHostWebView].window; });
        auto front = [inspector extensionHostWebView];
        evaluate(front, @"InspectorFrontendHost.requestSetDockSide = undefined; 'ok'");
        tick = [&] { tools.tick(); };
        tools.open();
        pump([&] { return tools.diagnostics()["embeddedSupported"] == false && !tools.diagnostics()["pending"].get<bool>(); });
        settle();
        CHECK(tools.visible() && front.window != window && front.window.visible);
        CHECK(tools.state()["mode"] == "embedded" && tools.diagnostics()["mode"] == "floating");
        CHECK(![menu(tools, @"Hide DevTools", @"Embed DevTools") itemAtIndex:1].enabled);
        CHECK(tools.diagnostics().contains("fallbackReason"));
        tools.hide(); settle(); CHECK(!tools.visible() && !front.window.visible);
        tools.open(); settle(); CHECK(tools.visible() && front.window.visible && [inspector isConnected]);
        tick = {};
      }
      auto unavailable = [[UnavailableInspectorView alloc] initWithFrame:window.contentView.bounds configuration:config];
      {
        MacDevTools tools(unavailable, [] {});
        CHECK(tools.diagnostics()["nativeToggleSupported"] == false && tools.diagnostics()["embeddedSupported"] == false);
        auto entries = menu(tools, @"Open DevTools", @"Embed DevTools");
        CHECK(![entries itemAtIndex:0].enabled && ![entries itemAtIndex:1].enabled);
        bool rejected = false;
        try { tools.open(); } catch (const Error& error) { rejected = error.code == "DEVTOOLS_UNAVAILABLE"; }
        CHECK(rejected);
      }
      [window close];
      std::cout << "WKWebView DevTools: Embedded/Floating, menus, shortcuts, hidden mode changes, Console/Elements retention, resizing, native close, host migration, two views, capability fallback and cleanup passed\n";
      return 0;
    } catch (const std::exception& error) { tick = {}; std::cerr << error.what() << '\n'; return 1; }
  }
}
