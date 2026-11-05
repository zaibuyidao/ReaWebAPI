#pragma once
#include "platform/shared/overlay.hpp"
#include <set>
#include <climits>
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <objc/runtime.h>

namespace reaweb {
class MacOverlay {
  __weak NSWindow* host_ = nil;
  __weak NSWindow* owner_ = nil;
  __weak WKWebView* view_ = nil;
  id observer_ = nil;
  id workspace_observer_ = nil;
  inline static Method dispatch_method_ = nullptr;
  inline static IMP application_dispatch_ = nullptr;
  inline static std::set<MacOverlay*> overlays_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  unsigned held_ = 0;
  bool minimized_ = false, wants_visible_ = true, order_pending_ = false, inactive_hidden_ = false;
  CFMachPortRef tap_ = nullptr;
  CFRunLoopSourceRef source_ = nullptr;
  OverlayOptions requested_;
  std::string applied_mode_ = "normal";
  bool pressed_ = false;
  inline static bool repairing_ = false;
  std::function<void(std::string)> error_;
  static NSArray<NSWindow*>* ordered_windows() {
    auto entries = (__bridge_transfer NSArray*)CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!entries) return nil;
    auto result = [NSMutableArray<NSWindow*> array];
    for (NSDictionary* entry in entries)
      if (auto window = [NSApp windowWithWindowNumber:[entry[(id)kCGWindowNumber] integerValue]]) [result addObject:window];
    return result;
  }
  static void send_event(id app, SEL selector, NSEvent* event) {
    reinterpret_cast<void(*)(id, SEL, NSEvent*)>(application_dispatch_)(app, selector, event);
    const bool mouse = event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeLeftMouseUp ||
      event.type == NSEventTypeRightMouseDown || event.type == NSEventTypeRightMouseUp ||
      event.type == NSEventTypeOtherMouseDown || event.type == NSEventTypeOtherMouseUp;
    if (mouse || event.type == NSEventTypeAppKitDefined || event.type == NSEventTypeApplicationDefined || event.type == NSEventTypeSystemDefined)
      for (auto item : overlays_) item->repair(mouse);
  }
  static bool buttons() {
    for (int i = 0; i < 5; ++i) if (CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, CGMouseButton(i))) return true;
    return false;
  }
  static CGEventRef event(CGEventTapProxy, CGEventType type, CGEventRef input, void* data) {
    auto& self = *static_cast<MacOverlay*>(data);
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
      self.error_("MOUSE_MODE_FAILED: The system disabled the native mouse event tap");
      return input;
    }
    const bool down = type == kCGEventLeftMouseDown || type == kCGEventRightMouseDown || type == kCGEventOtherMouseDown;
    const bool up = type == kCGEventLeftMouseUp || type == kCGEventRightMouseUp || type == kCGEventOtherMouseUp;
    if (!self.pressed_) self.route(CGEventGetLocation(input));
    const auto button = unsigned(CGEventGetIntegerValueField(input, kCGMouseEventButtonNumber));
    if (down) { self.held_ |= 1u << std::min(button, 31u); self.pressed_ = true; }
    if (up) {
      self.held_ &= ~(1u << std::min(button, 31u));
      auto alive = self.alive_; auto pointer = &self;
      CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
        if (*alive && !pointer->held_) { pointer->pressed_ = false; pointer->synchronize(); }
      });
    }
    return input;
  }
  void enable_mouse() {
    if (tap_) return;
    if (!AXIsProcessTrusted()) throw Error("MOUSE_MODE_UNAVAILABLE", "Native mouse routing requires Accessibility permission for REAPER");
    CGEventMask mask = 0;
    for (auto type : {kCGEventMouseMoved, kCGEventLeftMouseDown, kCGEventLeftMouseUp, kCGEventLeftMouseDragged,
        kCGEventRightMouseDown, kCGEventRightMouseUp, kCGEventRightMouseDragged, kCGEventOtherMouseDown,
        kCGEventOtherMouseUp, kCGEventOtherMouseDragged, kCGEventScrollWheel}) mask |= CGEventMaskBit(type);
    tap_ = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, mask, event, this);
    if (!tap_) throw Error("MOUSE_MODE_UNAVAILABLE", "Cannot establish a session mouse event tap. Check REAPER Accessibility permission");
    source_ = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap_, 0);
    if (!source_) { CFRelease(tap_); tap_ = nullptr; throw Error("MOUSE_MODE_FAILED", "Cannot attach the native mouse event tap"); }
    CFRunLoopAddSource(CFRunLoopGetMain(), source_, kCFRunLoopCommonModes);
    CGEventTapEnable(tap_, true);
    for (unsigned i = 0; i < 5; ++i)
      if (CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, CGMouseButton(i))) held_ |= 1u << i;
    pressed_ = held_ != 0;
  }
  void route(CGPoint location) {
    auto host = host_; auto view = view_;
    if (!host || !view) return;
    // Quartz uses the main display's upper-left origin. AppKit uses its lower-left origin.
    const auto height = CGDisplayBounds(CGMainDisplayID()).size.height;
    auto screen = NSMakePoint(location.x, height - location.y);
    auto point = [view convertPoint:[host convertPointFromScreen:screen] fromView:nil];
    const bool inside = NSPointInRect(point, view.bounds);
    const double zoom = view.pageZoom;
    const double x = (point.x - view.bounds.origin.x) / zoom;
    const double y = (view.flipped ? point.y - view.bounds.origin.y : NSMaxY(view.bounds) - point.y) / zoom;
    host.ignoresMouseEvents = inside && !mouse_hit(requested_.mode, requested_.regions, x, y);
    applied_mode_ = requested_.mode;
  }
public:
  ~MacOverlay() {
    *alive_ = false; overlays_.erase(this);
    if (observer_) [NSNotificationCenter.defaultCenter removeObserver:observer_];
    if (workspace_observer_) [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:workspace_observer_];
    if (overlays_.empty() && dispatch_method_ && method_getImplementation(dispatch_method_) == reinterpret_cast<IMP>(send_event)) {
      method_setImplementation(dispatch_method_, application_dispatch_);
      application_dispatch_ = nullptr; dispatch_method_ = nullptr;
    }
    if (source_) { CFRunLoopRemoveSource(CFRunLoopGetMain(), source_, kCFRunLoopCommonModes); CFRelease(source_); }
    if (tap_) { CFMachPortInvalidate(tap_); CFRelease(tap_); }
    host_.ignoresMouseEvents = NO;
  }
  static std::string mouse_unavailable() {
    return AXIsProcessTrusted() ? "" : "Requires Accessibility permission for REAPER";
  }
  void attach(NSWindow* host, NSWindow* owner, WKWebView* view, const OverlayOptions& options,
      std::function<void(std::string)> error) {
    host_ = host; owner_ = owner; view_ = view; requested_ = options; error_ = std::move(error);
    if (options.layer == "overlay") {
      if (!owner || owner == host) throw Error("LAYER_UNAVAILABLE", "REAPER main window unavailable");
      overlays_.insert(this);
      if (!application_dispatch_) {
        dispatch_method_ = class_getInstanceMethod(NSApp.class, @selector(sendEvent:));
        application_dispatch_ = method_setImplementation(dispatch_method_, reinterpret_cast<IMP>(send_event));
      }
      host.level = owner.level;
      host.hidesOnDeactivate = NO;
      workspace_observer_ = [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceDidActivateApplicationNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* notification) {
          NSRunningApplication* app = notification.userInfo[NSWorkspaceApplicationKey];
          if (app.processIdentifier == NSProcessInfo.processInfo.processIdentifier) schedule_repair();
        }];
      observer_ = [NSNotificationCenter.defaultCenter addObserverForName:nil object:nil queue:nil usingBlock:^(NSNotification* notification) {
        if (notification.object == NSApp) {
          if ([notification.name isEqualToString:NSApplicationDidResignActiveNotification] && host_.visible) {
            inactive_hidden_ = wants_visible_; [host_ orderOut:nil];
          }
          if ([notification.name isEqualToString:NSApplicationDidBecomeActiveNotification] && inactive_hidden_) {
            inactive_hidden_ = false;
            if (wants_visible_ && !owner_.miniaturized) [host_ orderFront:nil];
          }
        }
        if (notification.object == owner_) {
          if ([notification.name isEqualToString:NSWindowDidMiniaturizeNotification]) { minimized_ = host_.visible; if (minimized_) [host_ orderOut:nil]; }
          if ([notification.name isEqualToString:NSWindowDidDeminiaturizeNotification] && minimized_) {
            minimized_ = false; if (wants_visible_) [host_ orderFront:nil];
          }
        }
        if ([notification.object isKindOfClass:NSWindow.class] || notification.object == NSApp) schedule_repair();
        if (notification.object == host_ && ([notification.name isEqualToString:NSWindowDidMoveNotification] ||
            [notification.name isEqualToString:NSWindowDidResizeNotification] || [notification.name isEqualToString:NSWindowDidResignKeyNotification])) synchronize();
      }];
      repair();
    }
    if (options.mode != "normal") enable_mouse();
    synchronize();
  }
  void schedule_repair() {
    if (order_pending_ || repairing_) return;
    order_pending_ = true;
    auto alive = alive_; auto pointer = this;
    // AppKit commits activation ordering after dispatching window notifications.
    auto observer = CFRunLoopObserverCreateWithHandler(kCFAllocatorDefault, kCFRunLoopBeforeWaiting, false, LONG_MAX,
      ^(CFRunLoopObserverRef, CFRunLoopActivity) {
        if (*alive) { pointer->order_pending_ = false; pointer->repair(); }
      });
    if (!observer) { order_pending_ = false; error_("LAYER_FAILED: Cannot schedule native ordering"); return; }
    CFRunLoopAddObserver(CFRunLoopGetMain(), observer, kCFRunLoopCommonModes);
    CFRelease(observer);
    CFRunLoopWakeUp(CFRunLoopGetMain());
  }
  void repair(bool force = false) {
    auto host = host_; auto owner = owner_;
    if (requested_.layer != "overlay" || repairing_ || !host || !owner || !host.visible || !owner.visible || !NSApp.active) return;
    repairing_ = true;
    @try {
    if (host.level != owner.level) host.level = owner.level;
    auto windows = ordered_windows();
    if (!windows) { error_("LAYER_FAILED: Cannot inspect the native window order"); return; }
    auto own = [windows indexOfObject:owner];
    // Activating the main window can put same-level floating windows behind it.
    // Keep that group above the main window without changing levels or focus.
    NSWindow* lower_float = nil;
    if (own != NSNotFound) for (NSUInteger i = own + 1; i < windows.count; ++i) {
      NSWindow* candidate = windows[i];
      if (!candidate.visible || candidate.level != owner.level || candidate.frame.size.width <= 1 || candidate.frame.size.height <= 1) continue;
      bool overlay = false;
      for (auto item : overlays_) if (item->host_ == candidate) { overlay = true; break; }
      if (!overlay) lower_float = candidate;
    }
    if (lower_float) {
      [owner orderWindow:NSWindowBelow relativeTo:lower_float.windowNumber];
      windows = ordered_windows();
      if (!windows) { error_("LAYER_FAILED: Cannot inspect the native window order"); return; }
      own = [windows indexOfObject:owner];
    }
    const auto current = [windows indexOfObject:host];
    bool contiguous = current < own && current != NSNotFound && own != NSNotFound;
    if (contiguous) for (NSUInteger i = current + 1; i < own; ++i) {
      bool overlay = false;
      for (auto item : overlays_) if (item->host_ == windows[i]) { overlay = true; break; }
      if (!overlay) { contiguous = false; break; }
    }
    if (own != NSNotFound && current != NSNotFound && (force || !contiguous)) {
      NSWindow* upper_float = nil;
      for (NSUInteger i = 0; i < own; ++i) {
        NSWindow* candidate = windows[i];
        if (candidate.level != owner.level) continue;
        bool overlay = false;
        for (auto item : overlays_) if (item->host_ == candidate) { overlay = true; break; }
        if (!overlay) upper_float = candidate;
      }
      if (upper_float) [host orderWindow:NSWindowBelow relativeTo:upper_float.windowNumber];
      else [host orderWindow:NSWindowAbove relativeTo:owner.windowNumber];
    }
    } @catch (NSException* exception) {
      error_(std::string("LAYER_FAILED: ") + (exception.reason.UTF8String ?: "AppKit ordering failed"));
    } @finally { repairing_ = false; }
  }
  bool visibility(bool value) {
    wants_visible_ = value;
    if (requested_.layer != "overlay") return value;
    inactive_hidden_ = value && !NSApp.active;
    return value && NSApp.active && !owner_.miniaturized;
  }
  void synchronize() {
    if (pressed_ && buttons()) return;
    pressed_ = false;
    auto input = CGEventCreate(nullptr);
    if (input) { route(CGEventGetLocation(input)); CFRelease(input); }
  }
  void mode(const std::string& value) {
    if (value != "normal") enable_mouse();
    requested_.mode = value; synchronize();
  }
  void regions(const std::vector<MouseRegion>& value) { requested_.regions = value; synchronize(); }
  bool layer_active() const { return observer_ != nil; }
  bool mouse_active() const { return applied_mode_ == requested_.mode && (requested_.mode == "normal" || (tap_ && CGEventTapIsEnabled(tap_))); }
};
}
