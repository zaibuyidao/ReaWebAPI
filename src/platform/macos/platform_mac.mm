#include "platform/platform.hpp"
#include "platform/macos/mac_overlay.hpp"
#include "platform/shared/navigation.hpp"
#include "platform/shared/page_background.hpp"
#include "platform/shared/transparency.hpp"
#include "platform/shared/startup.hpp"
#include "platform/shared/idle_pool.hpp"
#include "platform/shared/drag_regions.hpp"
#include "platform/shared/window_menu.hpp"
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include "platform/shared/swell_window.hpp"
#include "platform/macos/mac_devtools.hpp"
#include "platform/macos/mac_resources.hpp"
#include "platform/macos/mac_video_presenter.hpp"
#include <fstream>
#include <cstring>

@interface WKPreferences (ReaWebVideoRefresh)
+ (NSArray*)_features;
- (void)_setEnabled:(BOOL)value forFeature:(id)feature;
@end

namespace {
void configure_video_refresh(WKPreferences* preferences) {
  @try {
    if (![WKPreferences respondsToSelector:@selector(_features)] ||
        ![preferences respondsToSelector:@selector(_setEnabled:forFeature:)]) return;
    for (id feature in [WKPreferences _features]) {
      if ([[feature valueForKey:@"key"] isEqualToString:@"PreferPageRenderingUpdatesNear60FPSEnabled"]) {
        [preferences _setEnabled:NO forFeature:feature];
        return;
      }
    }
  } @catch (NSException* error) { (void)error; }
}

void open_external(const std::string& url) {
  reaweb::validate_external_url(url);
  NSString* text = [[NSString alloc] initWithBytes:url.data() length:url.size() encoding:NSUTF8StringEncoding];
  if (![[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:text]])
    throw reaweb::Error("EXTERNAL_OPEN_FAILED", "The system could not open this link");
}
}

@interface ReaWebIdleDelegate : NSObject <WKNavigationDelegate> {
@public
  std::function<void(bool)> completed;
}
@end
@implementation ReaWebIdleDelegate
- (void)webView:(WKWebView*)view didFinishNavigation:(WKNavigation*)navigation {
  (void)view; (void)navigation; if (completed) completed(true);
}
- (void)webView:(WKWebView*)view didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {
  (void)view; (void)navigation; (void)error; if (completed) completed(false);
}
- (void)webView:(WKWebView*)view didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error {
  (void)view; (void)navigation; (void)error; if (completed) completed(false);
}
- (void)webViewWebContentProcessDidTerminate:(WKWebView*)view {
  (void)view; if (completed) completed(false);
}
@end

@interface ReaWebDelegate : NSObject <WKScriptMessageHandler, WKNavigationDelegate, WKUIDelegate> {
@public
  reaweb::WindowOptions options;
  std::string entryURI;
  std::string navigationURI;
  bool isClosed;
  NSOpenPanel* filePanel;
  std::function<void()> navigationStarted;
}
@end

@implementation ReaWebDelegate
// WKWebView exposes Pointer Lock authorization through this delegate selector.
- (void)_webViewDidRequestPointerLock:(WKWebView*)webView completionHandler:(void (^)(BOOL))completionHandler {
  completionHandler(!isClosed && !options.hosted && options.overlay.layer == "overlay" &&
    webView.window.isKeyWindow && reaweb::same_document(webView.URL.absoluteString.UTF8String ?: "", entryURI));
}
- (void)webView:(WKWebView*)webView runOpenPanelWithParameters:(WKOpenPanelParameters*)parameters
    initiatedByFrame:(WKFrameInfo*)frame completionHandler:(void (^)(NSArray<NSURL*>*))completionHandler {
  if (isClosed || filePanel || !frame.mainFrame || !webView.window) { completionHandler(nil); return; }
  filePanel = [NSOpenPanel openPanel];
  filePanel.canChooseFiles = YES;
  filePanel.canChooseDirectories = parameters.allowsDirectories;
  filePanel.allowsMultipleSelection = parameters.allowsMultipleSelection;
  [filePanel beginSheetModalForWindow:webView.window completionHandler:^(NSModalResponse response) {
    NSArray<NSURL*>* urls = !self->isClosed && response == NSModalResponseOK ? self->filePanel.URLs : nil;
    self->filePanel = nil;
    completionHandler(urls);
  }];
}
- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
  (void)controller;
  if (!isClosed && message.frameInfo.mainFrame && [message.body isKindOfClass:[NSString class]] &&
      reaweb::same_document(message.frameInfo.request.URL.absoluteString.UTF8String ?: "", entryURI))
    options.on_message([(NSString*)message.body UTF8String]);
}
- (void)webView:(WKWebView*)webView decidePolicyForNavigationAction:(WKNavigationAction*)action
    decisionHandler:(void (^)(WKNavigationActionPolicy))handler {
  const std::string uri = action.request.URL.absoluteString.UTF8String ?: "";
  bool allowed = action.targetFrame && action.targetFrame.mainFrame &&
    reaweb::same_document(uri, entryURI);
  const bool fragment = webView.URL && action.navigationType != WKNavigationTypeReload &&
    uri != navigationURI && reaweb::same_document(uri, navigationURI);
  if (allowed && !fragment && options.on_reload && options.on_reload()) allowed = false;
  if (allowed) navigationURI = uri;
  handler(allowed ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
  if (!allowed && !isClosed && action.targetFrame)
    reaweb::blocked_navigation(uri, entryURI, open_external, [webView](const std::string& script) {
      NSString* text = [[NSString alloc] initWithBytes:script.data() length:script.size() encoding:NSUTF8StringEncoding];
      [webView evaluateJavaScript:text completionHandler:nil];
    });
}
- (WKWebView*)webView:(WKWebView*)webView createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
    forNavigationAction:(WKNavigationAction*)action windowFeatures:(WKWindowFeatures*)features {
  (void)webView; (void)configuration; (void)action; (void)features;
  return nil;
}
- (void)webViewWebContentProcessDidTerminate:(WKWebView*)webView {
  (void)webView;
  isClosed = true;
  options.on_error("WEBVIEW_PROCESS_FAILED: WKWebView content process terminated; reopen the tool");
}
- (void)webView:(WKWebView*)webView didStartProvisionalNavigation:(WKNavigation*)navigation {
  (void)webView; (void)navigation;
  [filePanel cancel:nil];
  if (!isClosed && navigationStarted) navigationStarted();
  if (!isClosed && options.on_navigation) options.on_navigation();
}
- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
  (void)webView; (void)navigation;
  if (!isClosed && options.startup_timing) options.startup_timing->mark(reaweb::StartupTiming::Loaded);
}
- (void)webView:(WKWebView*)webView didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {
  (void)webView; (void)navigation;
  if (error.code != NSURLErrorCancelled) {
    isClosed = true;
    options.on_error(error.localizedDescription.UTF8String ?: "WKWebView navigation failed");
  }
}
@end

@interface ReaWebNativeView : WKWebView <NSDraggingSource> {
@public
  bool dropEnabled;
  bool sourceClosed;
  NSColor* resizeBackground;
  NSEvent* lastDragEvent;
  std::function<void(reaweb::Json)> receiveDrop;
  std::function<void(reaweb::Json)> dragReply;
  std::function<void()> toggleDock;
  std::function<bool()> isDocked;
  std::function<std::string()> appName;
  std::function<NSInteger(NSMenu*, NSInteger)> devtoolsMenu;
  std::function<void(int)> windowMenuAction;
  std::function<void()> videoLayoutChanged;
}
@end

@implementation ReaWebNativeView
- (void)viewDidMoveToSuperview {
  [super viewDidMoveToSuperview];
  if (videoLayoutChanged) videoLayoutChanged();
}
- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  if (videoLayoutChanged) videoLayoutChanged();
}
- (void)updateLayer {
  [super updateLayer];
  // Keep newly exposed pixels consistent while WebKit's resized tiles arrive.
  if (resizeBackground) self.layer.backgroundColor = resizeBackground.CGColor;
}
- (void)willOpenMenu:(NSMenu*)menu withEvent:(NSEvent*)event {
  [super willOpenMenu:menu withEvent:event];
  if (sourceClosed || !menu.numberOfItems) return;
  [menu removeAllItems];
  NSInteger index = 0;
  if (toggleDock) {
    const auto label = "Dock " + appName() + " in Docker";
    auto item = [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:label.c_str()]
      action:@selector(toggleDockFromMenu:) keyEquivalent:@""];
    item.target = self;
    item.state = isDocked && isDocked() ? NSControlStateValueOn : NSControlStateValueOff;
    [menu insertItem:item atIndex:index++];
  }
  const auto append = [&](const std::string& label, int command) {
    auto item = [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:label.c_str()] action:@selector(windowActionFromMenu:) keyEquivalent:@""];
    item.tag = command; item.target = self; [menu insertItem:item atIndex:index++];
  };
  if (windowMenuAction) append("Reload", 2);
  if (devtoolsMenu) index = devtoolsMenu(menu, index);
  if (windowMenuAction) { append("Open " + appName() + " Folder", 5); append("Close " + appName(), 6); }
}
- (void)windowActionFromMenu:(NSMenuItem*)sender { if (!sourceClosed && windowMenuAction) windowMenuAction(int(sender.tag)); }
- (void)toggleDockFromMenu:(id)sender {
  (void)sender;
  if (!sourceClosed && toggleDock) toggleDock();
}
- (NSDragOperation)draggingSession:(NSDraggingSession*)session sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
  (void)session; (void)context; return sourceClosed ? NSDragOperationNone : NSDragOperationCopy;
}
- (BOOL)ignoreModifierKeysForDraggingSession:(NSDraggingSession*)session { (void)session; return YES; }
- (void)draggingSession:(NSDraggingSession*)session endedAtPoint:(NSPoint)point operation:(NSDragOperation)operation {
  (void)session; (void)point;
  auto reply = std::move(dragReply);
  if (reply) try { reply({{"result", !sourceClosed && (operation & NSDragOperationCopy) != 0}}); } catch (...) {}
}
- (BOOL)acceptsNativeDrop:(id<NSDraggingInfo>)sender {
  if (!(sender.draggingSourceOperationMask & NSDragOperationCopy)) return NO;
  auto pasteboard = sender.draggingPasteboard;
  return [pasteboard canReadObjectForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey:@YES}] ||
    [pasteboard availableTypeFromArray:@[NSPasteboardTypeString]] != nil;
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
  if (!dropEnabled) return [super draggingEntered:sender];
  return [self acceptsNativeDrop:sender] ? NSDragOperationCopy : NSDragOperationNone;
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
  if (!dropEnabled) return [super draggingUpdated:sender];
  return [self acceptsNativeDrop:sender] ? NSDragOperationCopy : NSDragOperationNone;
}
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
  return dropEnabled ? [self acceptsNativeDrop:sender] : [super prepareForDragOperation:sender];
}
- (void)draggingExited:(id<NSDraggingInfo>)sender { if (!dropEnabled) [super draggingExited:sender]; }
- (void)concludeDragOperation:(id<NSDraggingInfo>)sender { if (!dropEnabled) [super concludeDragOperation:sender]; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
  if (!dropEnabled) return [super performDragOperation:sender];
  if (sourceClosed || !receiveDrop) return NO;
  try {
    auto urls = [sender.draggingPasteboard readObjectsForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey:@YES}];
    if (urls.count > 256) return NO;
    reaweb::Json files = reaweb::Json::array();
    for (NSURL* url in urls) if (url.fileURL) files.push_back(std::string(url.path.UTF8String ?: ""));
    const std::string text = [sender.draggingPasteboard stringForType:NSPasteboardTypeString].UTF8String ?: "";
    if (text.size() > reaweb::value_limit || (files.empty() && text.empty())) return NO;
    auto point = [self convertPoint:sender.draggingLocation fromView:nil];
    const double zoom = self.pageZoom;
    receiveDrop({{"files", files}, {"text", text}, {"x", (point.x - self.bounds.origin.x) / zoom},
      {"y", (self.flipped ? point.y - self.bounds.origin.y : NSMaxY(self.bounds) - point.y) / zoom}});
    return YES;
  } catch (const std::exception& error) { NSLog(@"ReaWebAPI drop: %s", error.what()); return NO; }
}
@end


namespace reaweb {
bool native_key_repeat() {
  auto event = NSApp.currentEvent;
  return event.type == NSEventTypeKeyDown && event.isARepeat;
}
namespace {
NSString* ns(const std::string& text) { return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding]; }
char dock_icon_owner;
class MacIdleView final : public std::enable_shared_from_this<MacIdleView> {
  ReaWebNativeView* view_;
  ReaWebIdleDelegate* delegate_;
public:
  bool ready = false, failed = false;
  MacIdleView(WKWebsiteDataStore* data, WKProcessPool* pool) {
    auto config = [WKWebViewConfiguration new];
    config.websiteDataStore = data; config.processPool = pool;
    [config setURLSchemeHandler:[ReaWebSchemeHandler new] forURLScheme:@"reaweb"];
    view_ = [[ReaWebNativeView alloc] initWithFrame:NSMakeRect(0, 0, 860, 640) configuration:config];
    delegate_ = [ReaWebIdleDelegate new]; view_.navigationDelegate = delegate_;
  }
  ~MacIdleView() { delegate_->completed = {}; view_.navigationDelegate = nil; [view_ stopLoading]; }
  void start() {
    const auto weak = weak_from_this();
    delegate_->completed = [weak](bool success) {
      if (auto self = weak.lock()) { self->ready = success; self->failed = !success; }
    };
    [view_ loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"about:blank"]]];
  }
  ReaWebNativeView* take() {
    delegate_->completed = {}; view_.navigationDelegate = nil;
    auto view = view_; view_ = nil; return view;
  }
};
class MacWindow final : public Window, public std::enable_shared_from_this<MacWindow> {
  std::unique_ptr<SwellWindow> window_;
  std::unique_ptr<MacOverlay> overlay_;
  ReaWebNativeView* webview_;
  std::shared_ptr<MacVideoPresenter> video_presenter_;
  NSColor* video_background_;
  NSColor* video_resize_background_;
  bool video_background_active_ = false;
  bool video_draws_background_ = true;
  void release_video_presenter() {
    webview_->videoLayoutChanged = {};
    if (video_presenter_) video_presenter_->invalidate();
    video_presenter_.reset();
    if (!video_background_active_) return;
    @try { [webview_ setValue:@(video_draws_background_) forKey:@"drawsBackground"]; }
    @catch (NSException* error) { (void)error; }
    webview_.underPageBackgroundColor = video_background_;
    webview_->resizeBackground = video_resize_background_;
    webview_.layer.backgroundColor = (video_resize_background_ ?: NSColor.clearColor).CGColor;
    webview_.needsDisplay = YES;
    video_background_active_ = false;
  }
  id mouse_monitor_;
  id focus_observer_;
  DragRegions drag_regions_;
  bool alpha_ready_ = false;
  double corner_radius_ = 0;
  bool rounding_applied_ = false;
  void update_corners(bool suspended = false) {
    auto content = (__bridge NSView*)GetDlgItem(static_cast<HWND>(window_->handle()), 0);
    auto native = floating_window();
    const bool active = !suspended && corner_radius_active();
    if (!active && !rounding_applied_) return;
    content.wantsLayer = YES;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    content.layer.cornerRadius = active ? corner_radius_ : 0;
    content.layer.masksToBounds = active;
    [CATransaction commit];
    if (native) {
      native.opaque = !(active || delegate_->options.transparent);
      native.backgroundColor = active || delegate_->options.transparent ? NSColor.clearColor : NSColor.windowBackgroundColor;
      SetOpaque(static_cast<HWND>(window_->handle()), !active && !delegate_->options.transparent);
      [native invalidateShadow];
    }
    rounding_applied_ = active;
  }
  static bool alpha_supported() {
    return [WKWebView instancesRespondToSelector:NSSelectorFromString(@"_setDrawsBackground:")];
  }
  NSWindow* floating_window() const {
    if (!window_) return nil;
    auto content = (__bridge NSView*)GetDlgItem(static_cast<HWND>(window_->handle()), 0);
    return content.window.contentView == content ? content.window : nil;
  }
  void set_host_visible(bool visible) {
    if (overlay_) visible = overlay_->visibility(visible);
    if (visible && !delegate_->options.hosted) if (auto native = floating_window()) {
      // SWELL may promote the first SW_SHOWNOACTIVATE to an activating show.
      native.contentView.hidden = NO;
      [native orderFront:nil]; if (overlay_) overlay_->repair(); return;
    }
    window_->set_visible(visible);
  }
  void configure_floating() {
    const auto& options = delegate_->options;
    if (options.hosted) return;
    auto native = floating_window();
    if (!native) throw Error("WINDOW_STYLE_FAILED", "The standalone AppKit host is unavailable");
    const auto frame = native.frame;
    auto style = native.styleMask;
    if (options.frameless) style &= ~(NSWindowStyleMaskTitled | NSWindowStyleMaskFullSizeContentView);
    else style |= NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    if (options.resizable) style |= NSWindowStyleMaskResizable;
    else style &= ~NSWindowStyleMaskResizable;
    native.styleMask = style;
    [native setFrame:frame display:NO];
    if (native.styleMask != style) throw Error("WINDOW_STYLE_FAILED", "AppKit could not apply the requested window style");
    if (options.transparent) {
      if (!alpha_supported()) throw Error("TRANSPARENCY_UNAVAILABLE", "WKWebView background control is unavailable");
      @try {
        [webview_ setValue:@NO forKey:@"drawsBackground"];
        if ([[webview_ valueForKey:@"drawsBackground"] boolValue])
          throw Error("TRANSPARENCY_FAILED", "WKWebView did not disable its opaque background");
      } @catch (NSException* exception) {
        throw Error("TRANSPARENCY_FAILED", exception.reason.UTF8String ?: "WKWebView background configuration failed");
      }
      native.opaque = NO;
      native.backgroundColor = NSColor.clearColor;
      native.ignoresMouseEvents = NO;
      webview_.underPageBackgroundColor = NSColor.clearColor;
      webview_->resizeBackground = NSColor.clearColor;
      webview_.wantsLayer = YES;
      webview_.layer.opaque = NO;
      webview_.layer.backgroundColor = NSColor.clearColor.CGColor;
      native.contentView.layer.opaque = NO;
      native.contentView.layer.backgroundColor = NSColor.clearColor.CGColor;
      SetOpaque(static_cast<HWND>(window_->handle()), false);
      window_->set_transparent(true);
      alpha_ready_ = !native.opaque;
      if (!alpha_ready_) throw Error("TRANSPARENCY_FAILED", "AppKit did not establish a non-opaque host");
    }
  }
  std::unique_ptr<MacDevTools> devtools_;
  ReaWebDelegate* delegate_;
  mutable Json normal_;
  bool maximized_ = false;
  bool controller_prewarmed_ = false;
  StartupPresentation startup_;
  __weak NSWindow* startup_key_ = NSApp.keyWindow;
  bool snapshot_pending_ = false;
  void mark_startup(StartupTiming::Stage stage) { if (delegate_->options.startup_timing) delegate_->options.startup_timing->mark(stage); }
  unsigned snapshot_requested_ = ~0u;
  void prepare_startup() {
    snapshot_requested_ = startup_.generation();
    if (!startup_.pending() || snapshot_pending_ || closed()) return;
    snapshot_pending_ = true;
    mark_startup(StartupTiming::FrameRequested);
    auto weak = weak_from_this();
    const auto generation = startup_.generation();
    auto configuration = [WKSnapshotConfiguration new];
    configuration.afterScreenUpdates = YES;
    [webview_ takeSnapshotWithConfiguration:configuration completionHandler:^(NSImage* image, NSError* error) {
      if (auto self = weak.lock(); self && !self->closed()) {
        self->snapshot_pending_ = false;
        if (generation != self->startup_.generation()) {
          if (self->snapshot_requested_ == self->startup_.generation()) self->prepare_startup();
        } else if (image && !error) { self->mark_startup(StartupTiming::FrameReady); self->startup_.prepared(generation); }
        else {
          self->delegate_->isClosed = true;
          self->delegate_->options.on_error("WKWebView could not prepare the initial frame");
        }
      }
    }];
  }
  NSImage* icon_ = nil;
  __weak NSWindow* icon_window_ = nil;
  bool icon_visible_ = true;
  bool icon_initialized_ = false;
  __weak NSWindow* dock_window_ = nil;
  NSURL* dock_url_ = nil;
  NSString* dock_filename_ = nil;
  NSImage* dock_image_ = nil;
  void release_dock_icon() {
    auto previous = dock_window_; dock_window_ = nil;
    auto owner = (NSValue*)objc_getAssociatedObject(previous, &dock_icon_owner);
    if (previous && owner.pointerValue == this) {
      objc_setAssociatedObject(previous, &dock_icon_owner, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
      previous.representedURL = dock_url_;
      if (!dock_url_) previous.representedFilename = dock_filename_ ?: @"";
      [previous standardWindowButton:NSWindowDocumentIconButton].image = dock_image_;
    }
    dock_url_ = nil; dock_filename_ = nil; dock_image_ = nil; icon_window_ = nil;
  }
  void apply_icon() {
    auto native = (__bridge NSWindow*)icon_target();
    const bool docked = delegate_->options.is_docked && delegate_->options.is_docked();
    auto shared = docked && (icon_ || !icon_visible_) ? native : nil;
    if (dock_window_ != shared || (dock_window_ && [(NSValue*)objc_getAssociatedObject(dock_window_, &dock_icon_owner) pointerValue] != this))
      release_dock_icon();
    if (shared && !dock_window_) {
      auto owner = (NSValue*)objc_getAssociatedObject(shared, &dock_icon_owner);
      if (auto previous = static_cast<MacWindow*>(owner.pointerValue)) previous->release_dock_icon();
      dock_url_ = shared.representedURL; dock_filename_ = shared.representedFilename;
      dock_image_ = [shared standardWindowButton:NSWindowDocumentIconButton].image;
      dock_window_ = shared;
      objc_setAssociatedObject(shared, &dock_icon_owner, [NSValue valueWithPointer:this], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
      icon_window_ = nil;
    }
    if (docked && !shared) return;
    if (!native) { icon_window_ = nil; return; }
    if (!icon_visible_ || (icon_initialized_ && !icon_)) {
      if (native.representedURL) native.representedURL = nil;
      icon_window_ = native; return;
    }
    if (!icon_) return;
    if (native == icon_window_ && native.representedURL && [native standardWindowButton:NSWindowDocumentIconButton].image == icon_) return;
    native.representedURL = [NSURL fileURLWithPath:ns(delegate_->options.entry.u8string())];
    auto button = [native standardWindowButton:NSWindowDocumentIconButton];
    button.image = icon_;
    icon_window_ = native;
  }
public:
  MacWindow(WindowOptions options, WKWebsiteDataStore* data, WKProcessPool* pool,
      std::shared_ptr<MacIdleView> prepared = {}) : startup_(options.hidden) {
    delegate_ = [ReaWebDelegate new];
    delegate_->options = std::move(options);
    mark_startup(StartupTiming::ControllerStart);
    delegate_->entryURI = delegate_->options.url.empty() ? file_uri(delegate_->options.entry) : delegate_->options.url;
    delegate_->navigationURI = delegate_->entryURI;
    delegate_->isClosed = false;
    WKWebViewConfiguration* config;
    if (prepared) {
      webview_ = prepared->take(); controller_prewarmed_ = true;
      config = webview_.configuration;
      auto handler = (ReaWebSchemeHandler*)[config urlSchemeHandlerForURLScheme:@"reaweb"];
      handler->resources = delegate_->options.resources;
    } else {
      config = [WKWebViewConfiguration new];
      config.websiteDataStore = data; config.processPool = pool;
      auto handler = [ReaWebSchemeHandler new]; handler->resources = delegate_->options.resources;
      [config setURLSchemeHandler:handler forURLScheme:@"reaweb"];
    }
    if (delegate_->options.video_high_refresh_rate) configure_video_refresh(config.preferences);
    config.preferences.elementFullscreenEnabled = YES;
    [config.userContentController addScriptMessageHandler:delegate_ name:@"reaweb"];
    auto script = [[WKUserScript alloc] initWithSource:ns(delegate_->options.script +
      (delegate_->options.transparent ? transparent_page_script("webkit.messageHandlers.reaweb.postMessage('__reawebPresent');") :
        page_background_script("webkit.messageHandlers.reaweb.postMessage(message);")) +
      drag_regions_script("webkit.messageHandlers.reaweb.postMessage(message);"))
      injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES];
    [config.userContentController addUserScript:script];
    if (!prepared) webview_ = [[ReaWebNativeView alloc] initWithFrame:NSMakeRect(0, 0, 860, 640) configuration:config];
    mark_startup(StartupTiming::ControllerReady);
    webview_->dropEnabled = false; webview_->sourceClosed = false;
    webview_->receiveDrop = delegate_->options.on_drop;
    webview_->toggleDock = delegate_->options.on_dock_toggle;
    webview_->isDocked = delegate_->options.is_docked;
    webview_->appName = delegate_->options.app_name ? delegate_->options.app_name :
      std::function<std::string()>([title = delegate_->options.title] { return title; });
    [webview_ registerForDraggedTypes:@[NSPasteboardTypeFileURL, NSPasteboardTypeString]];
    __weak ReaWebNativeView* weak_view = webview_;
    mouse_monitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskLeftMouseDragged | NSEventMaskKeyUp handler:^NSEvent*(NSEvent* event) {
      auto view = weak_view;
      if (event.type == NSEventTypeKeyUp) {
        const auto& routing = delegate_->options.routes_shortcuts;
        if (view && !view->sourceClosed && view.window.keyWindow && (event.modifierFlags & NSEventModifierFlagCommand) && routing && routing()) {
          int flags = 0;
          const int key = SWELL_MacKeyToWindowsKey((__bridge void*)event, &flags);
          const auto release = std::string("window.__reawebReleaseShortcut?.(") + std::to_string((flags & FVIRTKEY) ? key : 0) + "," + std::to_string((flags & FVIRTKEY) ? 0 : key) + ");";
          [view evaluateJavaScript:ns(release) completionHandler:nil];
        }
        return event;
      }
      if (view && event.window == view.window) {
        const auto point = [view convertPoint:event.locationInWindow fromView:nil];
        if (event.type == NSEventTypeLeftMouseDown && frameless_active() &&
            NSPointInRect(point, view.bounds) && NSPointInRect(point, view.visibleRect)) {
          const double zoom = view.pageZoom;
          const double x = (point.x - view.bounds.origin.x) / zoom;
          const double y = (view.flipped ? point.y - view.bounds.origin.y : NSMaxY(view.bounds) - point.y) / zoom;
          const auto frame = view.window.frame;
          const auto screen = [view.window convertPointToScreen:event.locationInWindow];
          const bool resize_edge = delegate_->options.resizable && !view.window.zoomed &&
            (screen.x < NSMinX(frame)+6 || screen.x >= NSMaxX(frame)-6 || screen.y < NSMinY(frame)+6 || screen.y >= NSMaxY(frame)-6);
          if (!resize_edge && drag_regions_.hit(x, y, view.bounds.size.width / zoom, view.bounds.size.height / zoom)) {
            if (event.clickCount == 2) {
              if (delegate_->options.resizable) [view.window zoom:nil];
            } else [view.window performWindowDragWithEvent:event];
            return nil;
          }
        }
        if (NSPointInRect(point, view.bounds)) view->lastDragEvent = event;
      }
      return event;
    }];
    webview_.navigationDelegate = delegate_;
    webview_.UIDelegate = delegate_;
    webview_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    window_ = std::make_unique<SwellWindow>(delegate_->options.title, delegate_->options.parent, [this] {
      const auto& capture = delegate_->options.captures_keyboard;
      if (!webview_->sourceClosed && capture && capture()) [webview_.window makeFirstResponder:webview_];
    }, delegate_->options.on_close, true, delegate_->options.hosted);
    delegate_->navigationStarted = [this] { release_video_presenter(); startup_.navigate(delegate_->options.hosted); drag_regions_.clear(); };
    delegate_->options.on_message = [this, receive = delegate_->options.on_message](std::string message) {
      if (message == "__reawebPresent") { prepare_startup(); return; }
      if (drag_regions_.message(message)) return;
      if (!page_background_message(message, [this](unsigned color) {
        if (delegate_->options.transparent) return;
        window_->set_background(color);
        auto background = [NSColor colorWithSRGBRed:((color >> 16) & 255) / 255.0
          green:((color >> 8) & 255) / 255.0 blue:(color & 255) / 255.0 alpha:1.0];
        if (video_background_active_) {
          video_background_ = video_resize_background_ = background;
          webview_.superview.layer.backgroundColor = background.CGColor;
          return;
        }
        webview_.underPageBackgroundColor = background;
        webview_->resizeBackground = background;
        webview_.needsDisplay = YES;
        webview_.superview.layer.backgroundColor = background.CGColor;
      })) receive(std::move(message));
    };
    auto content = (__bridge NSView*)GetDlgItem(static_cast<HWND>(window_->handle()), 0);
    content.wantsLayer = YES;
    webview_.frame = content.bounds;
    [content addSubview:webview_];
    window_->configure_floating = [this] { configure_floating(); };
    window_->resize = [this] { update_corners(); };
    try {
      configure_floating();
      if (delegate_->options.hosted && delegate_->options.overlay.floating_only())
        throw Error("WINDOW_HOSTED", "Overlay policies require a standalone App");
      if (!delegate_->options.hosted) {
        overlay_ = std::make_unique<MacOverlay>();
        auto owner = (__bridge NSView*)delegate_->options.parent;
        overlay_->attach(floating_window(), owner.window, webview_, delegate_->options.overlay,
          [this](std::string error) { delegate_->options.on_error(std::move(error)); });
      }
    }
    catch (...) {
      [NSEvent removeMonitor:mouse_monitor_]; mouse_monitor_ = nil;
      webview_.navigationDelegate = nil; webview_.UIDelegate = nil;
      [webview_.configuration.userContentController removeScriptMessageHandlerForName:@"reaweb"];
      throw;
    }
    focus_observer_ = [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidBecomeKeyNotification
      object:nil queue:nil usingBlock:^(NSNotification* notification) {
        auto view = weak_view;
        if (!view || view->sourceClosed || delegate_->options.hosted || notification.object != floating_window()) return;
        const auto& capture = delegate_->options.captures_keyboard;
        if (!capture || !capture()) return;
        auto responder = view.window.firstResponder;
        if (!responder || responder == view.superview || responder == view.window)
          [view.window makeFirstResponder:view];
      }];
    devtools_ = std::make_unique<MacDevTools>(webview_, [this] { if (!closed()) focus(); });
    webview_->devtoolsMenu = [this](NSMenu* menu, NSInteger index) { return devtools_->insert_menu(menu, index); };
    webview_->windowMenuAction = [this](int command) {
      const auto& options = delegate_->options;
      if (command == 2) { if (!options.on_reload || !options.on_reload()) reload(); }
      else if (command == 5) [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:ns(options.entry.parent_path().u8string())]];
      else if (command == 6 && options.on_close) options.on_close();
    };
    window_->context_menu = [this](LPARAM position) {
      auto keep_alive = shared_from_this();
      try {
        devtools_->tick();
        show_window_menu(static_cast<HWND>(window_->handle()), position, delegate_->options, devtools_->menu_state(),
          [this](DevToolsAction action) { devtools_->perform(action); }, [this] { reload(); });
      } catch (const std::exception& error) { delegate_->options.on_error(error.what()); }
    };
    if (delegate_->options.url.empty()) {
      auto url = [NSURL fileURLWithPath:ns(delegate_->options.entry.u8string())];
      [webview_ loadFileURL:url allowingReadAccessToURL:[url URLByDeletingLastPathComponent]];
    } else [webview_ loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:ns(delegate_->options.url)]]];
  }
  ~MacWindow() override {
    release_video_presenter();
    overlay_.reset();
    set_host_visible(false);
    if (focus_observer_) [NSNotificationCenter.defaultCenter removeObserver:focus_observer_];
    release_dock_icon();
    if (mouse_monitor_) [NSEvent removeMonitor:mouse_monitor_];
    webview_->devtoolsMenu = {};
    webview_->windowMenuAction = {};
    devtools_.reset();
    webview_->sourceClosed = true; webview_->dropEnabled = false; webview_->receiveDrop = {};
    webview_->toggleDock = {}; webview_->isDocked = {};
    auto drag_reply = std::move(webview_->dragReply);
    if (drag_reply) try { drag_reply({{"result", false}}); } catch (...) {}
    delegate_->isClosed = true;
    [delegate_->filePanel cancel:nil];
    delegate_->navigationStarted = {};
    [webview_ stopLoading];
    webview_.navigationDelegate = nil;
    webview_.UIDelegate = nil;
    [webview_.configuration.userContentController removeScriptMessageHandlerForName:@"reaweb"];
    [webview_ removeFromSuperview];
    window_.reset();
  }
  std::shared_ptr<VideoMemory> video_memory(const std::string& token, uint32_t bytes) override {
    auto handler = (ReaWebSchemeHandler*)[webview_.configuration urlSchemeHandlerForURLScheme:@"reaweb"];
    if (delegate_->isClosed || !handler || !handler->resources) return {};
    auto mapping = std::make_shared<MacVideoMemory>(bytes);
    mapping->resource_url = handler->resources->origin() + "/__reaweb_video__/" + token + "/";
    for (auto it = handler->videoSlots.begin(); it != handler->videoSlots.end();) {
      if (it->second.expired()) it = handler->videoSlots.erase(it); else ++it;
    }
    handler->videoSlots[token] = mapping;
    return mapping;
  }
  const char* video_presenter_backend() const override { return "metal"; }
  bool video_presenter_available() const override {
    return !closed() && !delegate_->options.hosted && mac_video_presenter_available() && alpha_supported();
  }
  std::shared_ptr<VideoPresenter> video_presenter(const Json& descriptor) override {
    if (!video_presenter_available()) throw Error("VIDEO_RENDERER_UNAVAILABLE", "Native video presentation is unavailable in this window");
    if (video_presenter_ && !video_presenter_->closed()) throw Error("VIDEO_SURFACE_IN_USE", "This window already has a native video surface");
    release_video_presenter();
    auto presenter = make_mac_video_presenter(webview_, descriptor);
    video_background_ = webview_.underPageBackgroundColor;
    video_resize_background_ = webview_->resizeBackground;
    @try {
      video_draws_background_ = [[webview_ valueForKey:@"drawsBackground"] boolValue];
      [webview_ setValue:@NO forKey:@"drawsBackground"];
      if ([[webview_ valueForKey:@"drawsBackground"] boolValue])
        throw Error("VIDEO_RENDERER_UNAVAILABLE", "WKWebView did not enable native video composition");
    } @catch (NSException* error) {
      @try { [webview_ setValue:@(video_draws_background_) forKey:@"drawsBackground"]; }
      @catch (NSException* restore_error) { (void)restore_error; }
      throw Error("VIDEO_RENDERER_UNAVAILABLE", error.reason.UTF8String ?: "WKWebView native video composition failed");
    }
    video_background_active_ = true;
    webview_.underPageBackgroundColor = NSColor.clearColor;
    webview_->resizeBackground = NSColor.clearColor;
    webview_.layer.backgroundColor = NSColor.clearColor.CGColor;
    video_presenter_ = presenter;
    std::weak_ptr<MacVideoPresenter> weak = presenter;
    webview_->videoLayoutChanged = [weak] { if (auto video = weak.lock()) video->update_view(); };
    return presenter;
  }
  void evaluate(const std::string& script) override {
    if (!closed()) [webview_ evaluateJavaScript:ns(script) completionHandler:nil];
  }
  void set_drop_enabled(bool enabled) override { webview_->dropEnabled = enabled; }
  void start_drag(const Json& payload, Reply reply) override {
    if (webview_->dragReply) throw Error("DRAG_BUSY", "A native drag is already active");
    if (!([NSEvent pressedMouseButtons] & 1) || !webview_->lastDragEvent)
      throw Error("DRAG_GESTURE_REQUIRED", "Hold the left mouse button in the WebView while starting a drag");
    NSMutableArray<NSDraggingItem*>* items = [NSMutableArray array];
    const auto point = [webview_ convertPoint:webview_->lastDragEvent.locationInWindow fromView:nil];
    if (!payload.at("files").empty()) {
      for (const auto& value : payload.at("files")) {
        auto path = ns(value.get<std::string>());
        auto item = [[NSDraggingItem alloc] initWithPasteboardWriter:[NSURL fileURLWithPath:path]];
        [item setDraggingFrame:NSMakeRect(point.x, point.y, 32, 32) contents:[[NSWorkspace sharedWorkspace] iconForFile:path]];
        [items addObject:item];
      }
    } else {
      auto writer = [NSPasteboardItem new];
      [writer setString:ns(payload.at("text").get<std::string>()) forType:NSPasteboardTypeString];
      auto item = [[NSDraggingItem alloc] initWithPasteboardWriter:writer];
      [item setDraggingFrame:NSMakeRect(point.x, point.y, 32, 32) contents:[NSImage imageNamed:NSImageNameMultipleDocuments]];
      [items addObject:item];
    }
    webview_->dragReply = std::move(reply);
    @try {
      auto session = [webview_ beginDraggingSessionWithItems:items event:webview_->lastDragEvent source:webview_];
      if (!session) { webview_->dragReply = {}; throw Error("DRAG_FAILED", "AppKit could not start native drag"); }
    } @catch (NSException* error) {
      webview_->dragReply = {}; throw Error("DRAG_FAILED", error.reason.UTF8String ?: "AppKit drag failed");
    }
  }
  void devtools() override { devtools_->open(); }
  Json devtools_state() const override { return devtools_->state(); }
  void restore_devtools(const Json& value) override { devtools_->restore(value); }
  bool closed() const override { return delegate_->isClosed || window_->closed(); }
  void* native_handle() const override { return window_->handle(); }
  unsigned background_color() const override { return window_->background_color(); }
  void reparent_host(void* parent) override {
    if (!delegate_->options.hosted) throw Error("HOST_UNAVAILABLE", "Expected a hosted window");
    set_visible(false);
    window_->reparent_host(parent);
    delegate_->options.parent = window_->host_parent();
  }
  void* icon_target() const override {
    auto native = webview_.window;
    if (delegate_->options.is_docked && delegate_->options.is_docked()) {
      auto main = (__bridge id)delegate_->options.parent;
      NSWindow* main_window = [main isKindOfClass:NSWindow.class] ? main : [main window];
      if (native == main_window || !window_->visible() || webview_.hiddenOrHasHiddenAncestor) return nullptr;
    }
    return (__bridge void*)native;
  }
  void prepare_undock() override { release_dock_icon(); }
  void prepare_dock() override {
    if (delegate_->options.overlay.floating_only()) throw Error("DOCK_UNAVAILABLE", "Overlay and mouse policies require a floating window");
    if (delegate_->options.transparent) throw Error("DOCK_UNAVAILABLE", "Transparent windows require a standalone floating host");
    update_corners(true);
    icon_window_ = nil;
    maximized_ = webview_.window.zoomed;
    if (maximized_) [webview_.window zoom:nil];
    window_->prepare_dock();
  }
  void restore_floating() override {
    icon_window_ = nil;
    window_->restore_floating(!startup_.pending());
    if (maximized_ && !webview_.window.zoomed) [webview_.window zoom:nil];
  }
  void focus() override {
    if (closed() || startup_.cancelled()) return;
    if (startup_.pending()) { startup_.show(true); startup_key_ = NSApp.keyWindow; tick(); }
    [NSApp activateIgnoringOtherApps:YES];
    window_->focus(); [webview_.window makeFirstResponder:webview_];
    if (overlay_) overlay_->repair();
  }
  void tick() override {
    if (closed()) return;
    if (video_presenter_) {
      if (video_presenter_->closed()) release_video_presenter();
      else video_presenter_->update_view();
    }
    if (startup_.expired()) {
      delegate_->isClosed = true;
      delegate_->options.on_error("WKWebView initial frame did not become available within 15 seconds"); return;
    }
    if (startup_.present()) {
      set_host_visible(true);
      if (!delegate_->options.hosted) mark_startup(StartupTiming::Revealed);
      if (maximized_ && !webview_.window.zoomed) [webview_.window zoom:nil];
      if (!delegate_->options.hosted && startup_.focus_requested() && NSApp.active && NSApp.keyWindow == startup_key_) {
        if (delegate_->options.on_present) delegate_->options.on_present();
        if (!(delegate_->options.is_docked && delegate_->options.is_docked())) focus();
      }
    }
    update_corners();
    devtools_->tick(); if (!delegate_->options.hosted) apply_icon();
  }
  void set_title(const std::string& title) override { window_->set_title(title); icon_window_ = nil; apply_icon(); }
  std::vector<int> icon_sizes() const override {
    const auto scale = std::clamp(webview_.window ? webview_.window.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor, 1.0, 8.0);
    return {int(std::lround(16 * scale)), int(std::lround(32 * scale))};
  }
  void set_icon(const std::vector<IconBitmap>& images) override {
    auto icon = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
    for (const auto& image : images) {
      auto rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:image.size pixelsHigh:image.size
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:image.size * 4 bitsPerPixel:32];
      if (!rep) throw Error("ICON_APPLY_FAILED", "Cannot allocate the native window icon");
      std::memcpy(rep.bitmapData, image.rgba.data(), image.rgba.size());
      rep.size = NSMakeSize(16, 16); [icon addRepresentation:rep];
    }
    icon_ = icon; icon_initialized_ = true; icon_window_ = nil; apply_icon();
  }
  void clear_icon() override { icon_ = nil; icon_initialized_ = true; icon_window_ = nil; apply_icon(); }
  void set_icon_visible(bool visible) override { icon_visible_ = visible; apply_icon(); }
  void set_visible(bool visible) override {
    startup_.request(visible);
    set_host_visible(visible && !startup_.pending());
  }
  void show() override {
    if (closed() || startup_.cancelled()) return;
    if (startup_.pending()) { startup_.show(); tick(); }
    else set_visible(true);
  }
  void without_focus(const std::function<void()>& action) override {
    NSWindow* key = NSApp.keyWindow;
    NSRunningApplication* active = NSWorkspace.sharedWorkspace.frontmostApplication;
    const auto restore = [&] {
      if (key && NSApp.keyWindow != key) [key makeKeyWindow];
      if (active && active.processIdentifier != NSProcessInfo.processInfo.processIdentifier && NSApp.active)
        [active activateWithOptions:0];
    };
    try { action(); } catch (...) { restore(); throw; }
    restore();
  }
  bool visibility_requested() const override { return startup_.pending() ? startup_.wanted() : visible(); }
  bool presentation_pending() const override { return startup_.pending(); }
  void cancel_pending_show() override { startup_.cancel(); }
  void show_after_create() override { if (startup_.pending()) window_->set_visible(false); }
  Json bounds() const override { return window_->placement(); }
  void reload() override { [webview_ reload]; }
  bool visible() const override { return !startup_.pending() && window_->visible() && !webview_.hiddenOrHasHiddenAncestor && !webview_.window.miniaturized; }
  bool focused() const override {
    auto responder = webview_.window.firstResponder;
    return webview_.window.keyWindow && [responder isKindOfClass:[NSView class]] &&
      [(NSView*)responder isDescendantOf:webview_];
  }
  Json placement() const override {
    if (startup_.pending() && !startup_.wanted() && !normal_.is_null()) return normal_;
    if (!webview_.window.zoomed) normal_ = window_->placement();
    auto value = webview_.window.zoomed && !normal_.is_null() ? normal_ : window_->placement();
    if (!value.is_null()) value["maximized"] = webview_.window.zoomed != NO;
    return value;
  }
  void restore_placement(const Json& value) override {
    normal_ = value;
    window_->restore_placement(value);
    maximized_ = value.value("maximized", false);
    if (maximized_ && window_->visible() && !webview_.window.zoomed) [webview_.window zoom:nil];
  }
  Json diagnostics() const override {
    return {{"backend", "WKWebView"}, {"controllerPrewarmed", controller_prewarmed_}, {"browserVersion", [[[NSBundle bundleForClass:[WKWebView class]] objectForInfoDictionaryKey:@"CFBundleVersion"] UTF8String] ?: "system"}, {"devtools", devtools_->diagnostics()}, {"windowFeatures", window_capabilities()}};
  }
  bool frameless_active() const override {
    auto native = floating_window();
    return delegate_->options.frameless && native && !(native.styleMask & NSWindowStyleMaskTitled);
  }
  void set_frameless(bool value) override {
    if (delegate_->options.frameless == value) return;
    update_corners(true);
    delegate_->options.frameless = value;
    if (auto native = floating_window()) {
      auto responder = native.firstResponder;
      configure_floating();
      [native makeFirstResponder:responder];
      update_corners();
      icon_window_ = nil; apply_icon();
    }
    evaluate(frameless_active() ? "window.__reawebDragRegionsActive?.(true);" : "window.__reawebDragRegionsActive?.(false);");
  }
  void set_corner_radius(double value) override { corner_radius_ = value; update_corners(); }
  bool corner_radius_active() const override {
    auto native = floating_window();
    return corner_radius_ > 0 && frameless_active() && (!delegate_->options.resizable || !native.zoomed) && !(native.styleMask & NSWindowStyleMaskFullScreen);
  }
  Json window_capabilities() const override {
    return {{"overlay", overlay_capabilities(delegate_->options.hosted ? "Hosted JSFX editor" : !delegate_->options.parent ? "REAPER main window unavailable" : "",
      delegate_->options.hosted ? "Hosted JSFX editor" : MacOverlay::mouse_unavailable(), "appkit-session-event-tap")}, {"hiddenStart", true}, {"frameless", true}, {"setFrameless", true}, {"cornerRadius", {{"supported", true}, {"framed", false}, {"frameless", true}, {"max", 64}}}, {"resizable", true}, {"dragRegions", true}, {"windowSystem", "appkit"},
      {"transparent", true}, {"transparency", transparency_capabilities(alpha_supported(), "appkit-wkwebview",
        alpha_supported() ? "" : "WKWebView background control is unavailable")}};
  }
  bool layer_active() const override { return overlay_ && overlay_->layer_active(); }
  bool mouse_mode_active() const override { return !overlay_ || overlay_->mouse_active(); }
  void set_mouse_mode(const std::string& value) override {
    if (!overlay_) throw Error("WINDOW_HOSTED", "Mouse policies require a standalone App");
    overlay_->mode(value); delegate_->options.overlay.mode = value;
  }
  void set_mouse_regions(const std::vector<MouseRegion>& value) override {
    if (!overlay_) throw Error("WINDOW_HOSTED", "Mouse policies require a standalone App");
    overlay_->regions(value); delegate_->options.overlay.regions = value;
  }
  void clear_mouse_regions() override { if (overlay_) set_mouse_regions({}); }
  bool transparent_active() const override {
    auto native = floating_window();
    return delegate_->options.transparent && alpha_ready_ && !closed() && native && !native.opaque;
  }
};
class MacPlatform final : public Platform {
  WKWebsiteDataStore* data_;
  WKProcessPool* pool_;
  IdlePool<MacIdleView> idle_;
public:
  explicit MacPlatform(const fs::path& path) {
    // Public WebKit APIs select a persistent profile; WebKit owns its physical storage location.
    const auto profile = path / "wk-profile-id";
    std::string identifier;
    std::ifstream input(profile);
    std::getline(input, identifier);
    NSUUID* uuid = identifier.empty() ? nil : [[NSUUID alloc] initWithUUIDString:ns(identifier)];
    if (!uuid && fs::exists(profile)) throw std::runtime_error("Invalid WKWebView shared profile id; browser data was left unchanged");
    if (!uuid) {
      uuid = [NSUUID UUID];
      std::ofstream output(profile, std::ios::trunc);
      output << uuid.UUIDString.UTF8String;
      output.close();
      if (!output) throw std::runtime_error("Cannot persist the WKWebView shared profile id");
    }
    data_ = [WKWebsiteDataStore dataStoreForIdentifier:uuid];
    pool_ = [WKProcessPool new];
  }
  void desktop(const std::string& method, const Json& args, DesktopReply reply) override {
    @autoreleasepool {
      if (method == "ReaWeb_GetDisplays") {
        Json displays = Json::array();
        for (NSScreen* screen in NSScreen.screens) {
          const auto rectangle = [](NSRect r) { return Json{{"x", r.origin.x}, {"y", r.origin.y}, {"width", r.size.width}, {"height", r.size.height}}; };
          displays.push_back({{"id", std::to_string([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue])},
            {"bounds", rectangle(screen.frame)}, {"workArea", rectangle(screen.visibleFrame)}, {"scaleFactor", screen.backingScaleFactor},
            {"dpi", 96 * screen.backingScaleFactor}, {"primary", screen == NSScreen.screens.firstObject}, {"units", "native"}});
        }
        reply({{"result", displays}}); return;
      }
      if (method == "ReaWeb_RevealPath") {
        auto url = [NSURL fileURLWithPath:ns(args.at(0).get<std::string>())];
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[url]];
        reply({{"result", true}}); return;
      }
      if (method == "ReaWeb_OpenExternal") {
        open_external(args[0].get<std::string>());
        reply({{"result", true}}); return;
      }
      auto pasteboard = [NSPasteboard generalPasteboard];
      if (method == "ReaWeb_ClipboardReadBinary" || method == "ReaWeb_ClipboardWriteBinary") {
        auto type = ns("ReaWebAPI.Binary:" + args.at(0).get<std::string>());
        if (method == "ReaWeb_ClipboardReadBinary") {
          NSData* data = [pasteboard dataForType:type];
          if (!data) { reply({{"result", nullptr}}); return; }
          if (data.length > value_limit) throw Error("BUFFER_LIMIT", "Clipboard exceeds 16 MiB");
          reply({{"result", encode_binary(static_cast<const char*>(data.bytes), data.length)}}); return;
        }
        const auto bytes = decode_binary(args.at(1));
        auto data = [NSData dataWithBytes:bytes.data() length:bytes.size()]; [pasteboard clearContents];
        if (![pasteboard setData:data forType:type]) throw Error("CLIPBOARD_ERROR", "Cannot write binary clipboard data");
        reply({{"result", true}}); return;
      }
      if (method == "ReaWeb_ClipboardReadText") {
        std::string value = [pasteboard stringForType:NSPasteboardTypeString].UTF8String ?: "";
        if (value.size() > value_limit) throw Error("BUFFER_LIMIT", "Clipboard exceeds 16 MiB");
        reply({{"result", value}});
      } else {
        [pasteboard clearContents];
        if (![pasteboard setString:ns(args[0].get<std::string>()) forType:NSPasteboardTypeString]) throw Error("CLIPBOARD_ERROR", "Cannot write clipboard");
        reply({{"result", true}});
      }
    }
  }
  std::shared_ptr<Window> open(WindowOptions options) override {
    @autoreleasepool {
      // This WebKit preference must be set before the video view is constructed.
      auto prepared = options.video_high_refresh_rate ? std::shared_ptr<MacIdleView>{} : idle_.take();
      return std::make_shared<MacWindow>(std::move(options), data_, pool_, std::move(prepared));
    }
  }
  void set_idle_capacity(unsigned count) override { idle_.capacity(count); }
  Json idle_diagnostics() const override { return idle_.diagnostics(); }
  void idle() override {
    @autoreleasepool {
      idle_.maintain([this] { auto view = std::make_shared<MacIdleView>(data_, pool_); view->start(); return view; });
    }
  }
};
}
std::unique_ptr<Platform> make_platform(const fs::path& data) { return std::make_unique<MacPlatform>(data); }
}
