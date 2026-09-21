#pragma once
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include "platform/shared/devtools.hpp"
#include <chrono>

// WebKit SPI is resolved per instance. No private framework headers or symbols are linked.
@protocol ReaWebInspectorSPI <NSObject>
- (void)show;
- (void)hide;
- (void)close;
- (void)detach;
- (BOOL)isVisible;
- (BOOL)isConnected;
- (WKWebView*)extensionHostWebView;
@end
@protocol ReaWebInspectableSPI <NSObject>
- (id<ReaWebInspectorSPI>)_inspector;
@end
@protocol ReaWebDeveloperPreferencesSPI <NSObject>
- (void)_setDeveloperExtrasEnabled:(BOOL)enabled;
@end

@interface ReaWebInspectorMenuTarget : NSObject <NSMenuItemValidation, WKScriptMessageHandler> {
@public
  std::function<void(reaweb::DevToolsAction)> perform;
  std::function<void(WKScriptMessage*)> receive;
}
- (void)devtoolsAction:(NSMenuItem*)sender;
@end
@implementation ReaWebInspectorMenuTarget
- (void)devtoolsAction:(NSMenuItem*)sender {
  if (perform) perform(static_cast<reaweb::DevToolsAction>(sender.tag));
}
- (BOOL)validateMenuItem:(NSMenuItem*)item { return perform && item.enabled; }
- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
  (void)controller;
  if (receive) receive(message);
}
@end

namespace reaweb {
class MacDevTools {
  using Clock = std::chrono::steady_clock;
  struct Lifetime { MacDevTools* owner; };
  WKWebView* view_;
  id<ReaWebInspectorSPI> inspector_ = nil;
  WKWebView* frontend_ = nil;
  NSWindow* native_window_ = nil;
  NSView* native_parent_ = nil;
  ReaWebInspectorMenuTarget* menu_target_;
  id key_monitor_ = nil;
  DevToolsPreferences prefs_;
  std::function<void()> focus_page_;
  std::shared_ptr<Lifetime> lifetime_ = std::make_shared<Lifetime>(Lifetime{this});
  bool supported_ = false, frontend_access_ = false, embedding_ = false;
  bool requested_ = false, pending_ = false, preparing_ = false, prepared_ = false;
  bool hosted_ = false, present_needed_ = false, seen_native_ = false;
  Clock::time_point deadline_{};
  std::string fallback_, error_;

  WKWebView* frontend() const { return frontend_access_ ? [inspector_ extensionHostWebView] : nil; }
  bool native_visible() const { return supported_ && [inspector_ isVisible]; }
  static void frame(NSView* view, NSRect rect) {
    if (view && !NSEqualRects(view.frame, rect)) view.frame = rect;
  }
  void layout() {
    const auto bounds = view_.superview.bounds;
    if (hosted_ && requested_ && native_visible()) {
      const auto scale = std::max(1.0, view_.window.backingScaleFactor);
      const auto width = std::round(bounds.size.width * prefs_.width_ratio * scale) / scale;
      const auto left = bounds.size.width - width;
      frame(view_, NSMakeRect(bounds.origin.x, bounds.origin.y, left, bounds.size.height));
      frame(frontend_, NSMakeRect(bounds.origin.x + left, bounds.origin.y, width, bounds.size.height));
    } else frame(view_, bounds);
  }
  void return_to_native() {
    if (!hosted_) return;
    [frontend_ removeFromSuperview];
    [native_parent_ addSubview:frontend_];
    frontend_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    frame(frontend_, native_parent_.bounds);
    frontend_.hidden = NO;
    hosted_ = false;
    layout();
  }
  void forget_frontend() {
    return_to_native();
    [frontend_.configuration.userContentController removeScriptMessageHandlerForName:@"reawebInspector"];
    frontend_ = nil; native_window_ = nil; native_parent_ = nil;
    prepared_ = preparing_ = false;
  }
  void prepare_frontend() {
    auto front = frontend();
    if (!front || !front.window) return;
    // Keep WebKit's native window as the floating owner. AppKit manages compact panel geometry.
    if (front.window == view_.window) { [inspector_ detach]; return; }
    frontend_ = front; native_window_ = front.window; native_parent_ = front.superview;
    if (!requested_) [native_window_ orderOut:nil];
    auto controller = front.configuration.userContentController;
    [controller removeScriptMessageHandlerForName:@"reawebInspector"];
    [controller addScriptMessageHandler:menu_target_ name:@"reawebInspector"];
    preparing_ = true;
    auto life = lifetime_;
    [front evaluateJavaScript:@"(() => { "
      "const h = globalThis.InspectorFrontendHost, ui = globalThis.WI; "
      "if (!h || typeof h.requestSetDockSide !== 'function' || typeof h.setAttachedWindowWidth !== 'function' || "
        "!ui || typeof ui.updateDockedState !== 'function' || typeof ui.updateDockingAvailability !== 'function') return false; "
      "if (!globalThis.__reawebInspector) { "
        "const send = body => { webkit.messageHandlers.reawebInspector.postMessage(body); }; "
        "globalThis.__reawebInspector = {dock: h.requestSetDockSide, width: h.setAttachedWindowWidth, availability: ui.updateDockingAvailability}; "
        "h.requestSetDockSide = side => send({mode: side === 'undocked' ? 'floating' : 'embedded'}); "
        "h.setAttachedWindowWidth = width => send({width}); "
        "ui.updateDockingAvailability = () => __reawebInspector.availability(true); "
      "} ui.updateDockingAvailability(true); return true; })()"
      completionHandler:^(id value, NSError* error) {
        auto self = life->owner;
        if (!self || self->frontend_ != front) return;
        self->preparing_ = false; self->prepared_ = true;
        if (error || ![value isKindOfClass:[NSNumber class]] || ![value boolValue]) {
          self->embedding_ = false;
          self->fallback_ = "WebKit Inspector presentation controls are unavailable; using the native floating window";
          [front.configuration.userContentController removeScriptMessageHandlerForName:@"reawebInspector"];
        }
        self->present_needed_ = true;
      }];
  }
  void present() {
    present_needed_ = false;
    if (!frontend_access_) {
      if (requested_) { [inspector_ detach]; [inspector_ show]; } else [inspector_ hide];
      return;
    }
    if (!prepared_ || !frontend_) return;
    const bool embed = embedding_ && !prefs_.floating;
    if (embed) {
      [native_window_ orderOut:nil];
      if (!hosted_) {
        [frontend_ removeFromSuperview];
        [view_.superview addSubview:frontend_];
        frontend_.autoresizingMask = NSViewNotSizable;
        hosted_ = true;
      }
      frontend_.hidden = !requested_;
    } else {
      return_to_native();
      if (requested_) [native_window_ makeKeyAndOrderFront:nil]; else [native_window_ orderOut:nil];
    }
    layout();
    if (embedding_) [frontend_ evaluateJavaScript:embed ? @"WI.updateDockedState('right')" : @"WI.updateDockedState('undocked')" completionHandler:nil];
    if (requested_) [frontend_.window makeFirstResponder:frontend_];
  }
  void safely_perform(DevToolsAction action) {
    try { perform(action); }
    catch (const std::exception& error) { error_ = error.what(); }
  }
public:
  MacDevTools(WKWebView* view, std::function<void()> focus_page)
    : view_(view), focus_page_(std::move(focus_page)) {
    view_.inspectable = YES;
    auto preferences = (id<ReaWebDeveloperPreferencesSPI>)view_.configuration.preferences;
    if ([preferences respondsToSelector:@selector(_setDeveloperExtrasEnabled:)] &&
        [view_ respondsToSelector:@selector(_inspector)]) {
      [preferences _setDeveloperExtrasEnabled:YES];
      inspector_ = [(id<ReaWebInspectableSPI>)view_ _inspector];
      supported_ = inspector_ != nil;
      for (auto selector : {@selector(show), @selector(hide), @selector(close), @selector(detach),
          @selector(isVisible), @selector(isConnected)})
        supported_ = supported_ && [inspector_ respondsToSelector:selector];
    }
    frontend_access_ = supported_ && [inspector_ respondsToSelector:@selector(extensionHostWebView)];
    embedding_ = frontend_access_;
    if (!supported_) fallback_ = "Native Web Inspector controls are unavailable. Inspect this page from Safari's Develop menu";
    else if (!embedding_) fallback_ = "This WebKit version does not expose the Inspector view; using the native floating window";
    menu_target_ = [ReaWebInspectorMenuTarget new];
    menu_target_->perform = [this](DevToolsAction action) { safely_perform(action); };
    menu_target_->receive = [this](WKScriptMessage* message) {
      if (message.webView != frontend_ || !message.frameInfo.mainFrame || ![message.body isKindOfClass:[NSDictionary class]]) return;
      id mode = message.body[@"mode"];
      if ([mode isEqual:@"floating"]) safely_perform(DevToolsAction::Float);
      else if ([mode isEqual:@"embedded"]) safely_perform(DevToolsAction::Embed);
      id width = message.body[@"width"];
      const auto available = view_.superview.bounds.size.width;
      if (hosted_ && requested_ && [width isKindOfClass:[NSNumber class]] && available > 0 && std::isfinite([width doubleValue])) {
        prefs_.width_ratio = std::clamp([width doubleValue] / available, 0.2, 0.8); layout();
      }
    };
    key_monitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* event) {
      auto front = frontend();
      auto responder = event.window.firstResponder;
      const bool page_focused = event.window == view_.window && [responder isKindOfClass:[NSView class]] &&
        [(NSView*)responder isDescendantOf:view_];
      const bool inspector_focused = front.window && event.window == front.window &&
        [responder isKindOfClass:[NSView class]] && [(NSView*)responder isDescendantOf:front];
      const auto modifiers = event.modifierFlags &
        (NSEventModifierFlagControl | NSEventModifierFlagShift | NSEventModifierFlagCommand | NSEventModifierFlagOption);
      if ((page_focused || inspector_focused) &&
          [event.charactersIgnoringModifiers.lowercaseString isEqualToString:@"i"] &&
          modifiers == (NSEventModifierFlagOption | NSEventModifierFlagCommand)) {
        if (!event.isARepeat) safely_perform(menu_state().shown ? DevToolsAction::Hide : DevToolsAction::Open);
        return nil;
      }
      return event;
    }];
  }
  ~MacDevTools() {
    lifetime_->owner = nullptr;
    if (key_monitor_) [NSEvent removeMonitor:key_monitor_];
    menu_target_->perform = {}; menu_target_->receive = {};
    forget_frontend();
    if (supported_) [inspector_ close];
  }
  bool visible() const { return requested_ && native_visible(); }
  void open() {
    if (!supported_) throw Error("DEVTOOLS_UNAVAILABLE", fallback_);
    requested_ = true; error_.clear();
    if (prepared_ && native_visible()) { present(); return; }
    if (prepared_) forget_frontend();
    if (!pending_) {
      pending_ = true;
      deadline_ = Clock::now() + std::chrono::seconds(10);
      [inspector_ show];
    }
  }
  void hide() {
    requested_ = false;
    if (frontend_access_) {
      if (hosted_) frontend_.hidden = YES;
      [native_window_ orderOut:nil]; layout();
    } else if (supported_) [inspector_ hide];
    focus_page_();
  }
  void perform(DevToolsAction action) {
    if (action == DevToolsAction::Open) open();
    else if (action == DevToolsAction::Hide) hide();
    else {
      prefs_.floating = action == DevToolsAction::Float;
      present_needed_ = true;
    }
  }
  void tick() {
    if (!supported_) return;
    const bool native_shown = native_visible();
    if (!native_shown && seen_native_) {
      requested_ = false; forget_frontend(); layout();
    }
    if (native_shown && !seen_native_ && !pending_) requested_ = true;
    seen_native_ = native_shown;
    if (!native_shown) {
      if (pending_ && Clock::now() >= deadline_) {
        pending_ = requested_ = false;
        error_ = "WebKit did not open Web Inspector within 10 seconds";
        [inspector_ close];
      }
      return;
    }
    if (pending_) { pending_ = false; present_needed_ = true; }
    if (frontend_access_) {
      if (!prepared_) { if (!preparing_) prepare_frontend(); return; }
      if (native_window_.visible && (hosted_ || !requested_)) {
        requested_ = true; present_needed_ = true;
      }
      if (present_needed_) present();
      layout();
    } else if (present_needed_) present();
  }
  DevToolsMenuState menu_state() const {
    return {pending_ ? requested_ : visible(), prefs_.floating || !embedding_, embedding_};
  }
  NSInteger insert_menu(NSMenu* menu, NSInteger index) const {
    auto state = menu_state();
    auto add = [&](NSString* title, DevToolsAction action, bool enabled, bool shortcut) {
      auto item = [[NSMenuItem alloc] initWithTitle:title action:@selector(devtoolsAction:) keyEquivalent:shortcut ? @"i" : @""];
      item.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
      item.target = menu_target_; item.tag = static_cast<NSInteger>(action); item.enabled = enabled;
      [menu insertItem:item atIndex:index++];
    };
    add(state.shown ? @"Hide DevTools" : @"Open DevTools", state.shown ? DevToolsAction::Hide : DevToolsAction::Open, supported_, true);
    add(state.floating ? @"Embed DevTools" : @"Float DevTools", state.floating ? DevToolsAction::Embed : DevToolsAction::Float,
      supported_ && (!state.floating || state.embedded_supported), false);
    return index;
  }
  Json state() const { return prefs_.state(); }
  void restore(const Json& value) { prefs_.restore(value); present_needed_ = true; }
  Json diagnostics() const {
    auto value = prefs_.state();
    value.update({{"mode", menu_state().floating ? "floating" : "embedded"}, {"visible", visible()},
      {"pending", pending_ || preparing_}, {"embeddedSupported", embedding_}, {"nativeToggleSupported", supported_}});
    if (!fallback_.empty()) value["fallbackReason"] = fallback_;
    if (!error_.empty()) value["lastError"] = error_;
    return value;
  }
};
}
