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

@interface ReaWebInspectorMenuTarget : NSObject <NSMenuItemValidation> {
@public
  std::function<void(reaweb::DevToolsAction)> perform;
}
- (void)devtoolsAction:(NSMenuItem*)sender;
@end
@implementation ReaWebInspectorMenuTarget
- (void)devtoolsAction:(NSMenuItem*)sender {
  if (perform) perform(static_cast<reaweb::DevToolsAction>(sender.tag));
}
- (BOOL)validateMenuItem:(NSMenuItem*)item { return perform && item.enabled; }
@end

namespace reaweb {
class MacDevTools {
  using Clock = std::chrono::steady_clock;
  struct Lifetime { MacDevTools* owner; };
  WKWebView* view_;
  id<ReaWebInspectorSPI> inspector_ = nil;
  ReaWebInspectorMenuTarget* menu_target_;
  id key_monitor_ = nil;
  DevToolsPreferences prefs_;
  std::function<void()> focus_page_;
  std::shared_ptr<Lifetime> lifetime_ = std::make_shared<Lifetime>(Lifetime{this});
  bool supported_ = false, frontend_access_ = false, embedding_ = false, requested_ = false, pending_ = false;
  bool seen_visible_ = false, layout_needed_ = false, layout_busy_ = false, settling_ = false;
  bool expected_floating_ = false, observed_floating_ = false;
  NSSize host_size_ = NSZeroSize;
  NSRect inspector_frame_ = NSZeroRect;
  Clock::time_point deadline_{}, settle_after_{}, settle_deadline_{};
  std::string fallback_, error_;

  WKWebView* frontend() const { return frontend_access_ ? [inspector_ extensionHostWebView] : nil; }
  bool floating() const {
    if (!frontend_access_) return true;
    auto front = frontend();
    return front.window && front.window != view_.window;
  }
  bool fits() const {
    const auto size = view_.superview.bounds.size;
    return size.width >= 820 && size.height >= 334;
  }
  std::string fallback_reason() const {
    if (!supported_ || !embedding_) return fallback_;
    if (!prefs_.floating && !fits()) return "The WebView is too small for WebKit's native Inspector layout (820 x 334 points required)";
    return {};
  }
  bool on_right() const {
    auto front = frontend();
    const auto bounds = view_.superview.bounds;
    return front && front.superview == view_.superview && front.window == view_.window &&
      std::abs(NSMinX(front.frame) - NSMaxX(view_.frame)) <= 1 &&
      std::abs(NSMaxX(front.frame) - NSMaxX(bounds)) <= 1 &&
      std::abs(NSHeight(front.frame) - NSHeight(bounds)) <= 1;
  }
  void remember_layout() {
    host_size_ = view_.superview.bounds.size;
    inspector_frame_ = frontend().frame;
    observed_floating_ = floating();
  }
  void apply_layout() {
    if (!embedding_) {
      [inspector_ detach]; layout_needed_ = false; remember_layout(); return;
    }
    auto front = frontend();
    if (!front || !visible()) return;
    expected_floating_ = prefs_.floating || !fallback_reason().empty();
    const auto width = std::lround(view_.superview.bounds.size.width * prefs_.width_ratio);
    auto script = [NSString stringWithFormat:
      @"(() => { const h = globalThis.InspectorFrontendHost; "
       "if (!h || typeof h.requestSetDockSide !== 'function' || typeof h.setAttachedWindowWidth !== 'function') return false; "
       "h.requestSetDockSide('%@'); %@ return true; })()",
      expected_floating_ ? @"undocked" : @"right",
      expected_floating_ ? @"" : [NSString stringWithFormat:@"h.setAttachedWindowWidth(%ld);", width]];
    layout_needed_ = false; layout_busy_ = true; settling_ = true;
    settle_deadline_ = Clock::now() + std::chrono::seconds(3);
    auto life = lifetime_;
    [front evaluateJavaScript:script completionHandler:^(id value, NSError* error) {
      auto self = life->owner;
      if (!self) return;
      self->layout_busy_ = false;
      if (error || ![value isKindOfClass:[NSNumber class]] || ![value boolValue]) {
        self->embedding_ = false;
        self->fallback_ = "WebKit Inspector docking controls are unavailable; using the native floating window";
        self->expected_floating_ = true;
        [self->inspector_ detach];
      }
      self->settle_after_ = Clock::now() + std::chrono::milliseconds(100);
      if (!self->requested_) [self->inspector_ hide];
    }];
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
    menu_target_->perform = {};
    if (supported_) [inspector_ close];
  }
  bool visible() const { return supported_ && [inspector_ isVisible]; }
  void open() {
    if (!supported_) throw Error("DEVTOOLS_UNAVAILABLE", fallback_);
    requested_ = true; error_.clear();
    if (!pending_) {
      pending_ = true;
      deadline_ = Clock::now() + std::chrono::seconds(10);
      [inspector_ show];
    }
  }
  void hide() {
    requested_ = false;
    if (supported_) [inspector_ hide];
    seen_visible_ = false;
    focus_page_();
  }
  void perform(DevToolsAction action) {
    if (action == DevToolsAction::Open) open();
    else if (action == DevToolsAction::Hide) hide();
    else {
      prefs_.floating = action == DevToolsAction::Float;
      if (visible() || pending_) layout_needed_ = true;
    }
  }
  void tick() {
    if (!supported_) return;
    auto shown = visible();
    if (pending_) {
      if (shown) {
        pending_ = false;
        if (!requested_) { [inspector_ hide]; shown = false; }
        else layout_needed_ = true;
      } else if (Clock::now() >= deadline_) {
        pending_ = requested_ = false;
        error_ = "WebKit did not open Web Inspector within 10 seconds";
        [inspector_ close];
      }
    } else if (shown && !seen_visible_) {
      requested_ = true; layout_needed_ = true;
    } else if (!shown && seen_visible_) requested_ = false;
    seen_visible_ = shown;
    if (!shown || layout_busy_) return;
    if (settling_) {
      const bool placed = expected_floating_ ? floating() : on_right();
      if (Clock::now() < settle_after_ || (!placed && Clock::now() < settle_deadline_)) return;
      settling_ = false;
      if (!placed) {
        embedding_ = false;
        fallback_ = "WebKit could not attach the Inspector on the right; using the native floating window";
        [inspector_ detach];
      }
      remember_layout();
    }
    if (!layout_needed_) {
      auto size = view_.superview.bounds.size;
      if (floating() != observed_floating_) {
        prefs_.floating = floating();
        layout_needed_ = !prefs_.floating;
      } else if (!NSEqualSizes(size, host_size_)) {
        layout_needed_ = !prefs_.floating;
      } else if (!floating() && !on_right()) layout_needed_ = true;
      else if (!floating() && size.width > 0 && !NSEqualRects(frontend().frame, inspector_frame_))
        prefs_.width_ratio = std::clamp(frontend().frame.size.width / size.width, 0.2, 0.8);
    }
    if (layout_needed_) apply_layout();
    else remember_layout();
  }
  DevToolsMenuState menu_state() const {
    return {pending_ ? requested_ : visible(), visible() && !layout_needed_ && !settling_ ? floating() :
      (prefs_.floating || !fallback_reason().empty()), embedding_ && fits()};
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
  void restore(const Json& value) {
    prefs_.restore(value);
    if (visible() || pending_) layout_needed_ = true;
  }
  Json diagnostics() const {
    auto value = prefs_.state();
    value.update({{"mode", menu_state().floating ? "floating" : "embedded"}, {"visible", visible()},
      {"pending", pending_}, {"embeddedSupported", embedding_ && fits()}, {"nativeToggleSupported", supported_}});
    const auto reason = fallback_reason();
    if (!reason.empty()) value["fallbackReason"] = reason;
    if (!error_.empty()) value["lastError"] = error_;
    return value;
  }
};
}
