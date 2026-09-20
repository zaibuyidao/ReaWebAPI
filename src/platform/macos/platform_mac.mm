#include "platform/platform.hpp"
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include "platform/shared/swell_window.hpp"
#include "platform/shared/devtools.hpp"
#include <fstream>

@interface ReaWebDelegate : NSObject <WKScriptMessageHandler, WKNavigationDelegate, WKUIDelegate> {
@public
  reaweb::WindowOptions options;
  std::string entryURI;
  std::string navigationURI;
  bool isClosed;
}
@end

@implementation ReaWebDelegate
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
}
- (WKWebView*)webView:(WKWebView*)webView createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
    forNavigationAction:(WKNavigationAction*)action windowFeatures:(WKWindowFeatures*)features {
  (void)webView; (void)configuration; (void)action; (void)features;
  return nil;
}
- (void)webViewWebContentProcessDidTerminate:(WKWebView*)webView {
  (void)webView;
  isClosed = true;
  options.on_error("WKWebView content process terminated; reopen the tool");
}
- (void)webView:(WKWebView*)webView didStartProvisionalNavigation:(WKNavigation*)navigation {
  (void)webView; (void)navigation;
  if (!isClosed && options.on_navigation) options.on_navigation();
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
  NSEvent* lastDragEvent;
  std::function<void(reaweb::Json)> receiveDrop;
  std::function<void(reaweb::Json)> dragReply;
}
@end

@implementation ReaWebNativeView
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
namespace {
NSString* ns(const std::string& text) { return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding]; }
class MacWindow final : public Window {
  std::unique_ptr<SwellWindow> window_;
  ReaWebNativeView* webview_;
  id mouse_monitor_;
  id key_monitor_;
  NSPanel* inspector_help_ = nil;
  DevToolsPreferences devtools_prefs_;
  ReaWebDelegate* delegate_;
  mutable Json normal_;
  bool maximized_ = false;
public:
  MacWindow(WindowOptions options, WKWebsiteDataStore* data, WKProcessPool* pool) {
    delegate_ = [ReaWebDelegate new];
    delegate_->options = std::move(options);
    delegate_->entryURI = delegate_->options.url.empty() ? file_uri(delegate_->options.entry) : delegate_->options.url;
    delegate_->navigationURI = delegate_->entryURI;
    delegate_->isClosed = false;
    auto config = [WKWebViewConfiguration new];
    config.websiteDataStore = data;
    config.processPool = pool;
    [config.userContentController addScriptMessageHandler:delegate_ name:@"reaweb"];
    auto script = [[WKUserScript alloc] initWithSource:ns(delegate_->options.script)
      injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES];
    [config.userContentController addUserScript:script];
    webview_ = [[ReaWebNativeView alloc] initWithFrame:NSMakeRect(0, 0, 860, 640) configuration:config];
    webview_->dropEnabled = false; webview_->sourceClosed = false;
    webview_->receiveDrop = delegate_->options.on_drop;
    [webview_ registerForDraggedTypes:@[NSPasteboardTypeFileURL, NSPasteboardTypeString]];
    __weak ReaWebNativeView* weak_view = webview_;
    mouse_monitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskLeftMouseDragged handler:^NSEvent*(NSEvent* event) {
      auto view = weak_view;
      if (view && event.window == view.window) {
        const auto point = [view convertPoint:event.locationInWindow fromView:nil];
        if (NSPointInRect(point, view.bounds)) view->lastDragEvent = event;
      }
      return event;
    }];
    webview_.navigationDelegate = delegate_;
    webview_.UIDelegate = delegate_;
    webview_.inspectable = YES;
    webview_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    window_ = std::make_unique<SwellWindow>(delegate_->options.title, delegate_->options.parent, std::function<void()>{}, delegate_->options.on_close);
    auto content = (__bridge NSView*)GetDlgItem(static_cast<HWND>(window_->handle()), 0);
    webview_.frame = content.bounds;
    [content addSubview:webview_];
    devtools_prefs_.floating = true;
    key_monitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* event) {
      const auto flags = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
      const auto modifiers = flags & (NSEventModifierFlagControl | NSEventModifierFlagShift | NSEventModifierFlagCommand | NSEventModifierFlagOption);
      auto responder = webview_.window.firstResponder;
      const bool page_focused = event.window == webview_.window && [responder isKindOfClass:[NSView class]] &&
        [(NSView*)responder isDescendantOf:webview_];
      if ((page_focused || event.window == inspector_help_) &&
          [event.charactersIgnoringModifiers.lowercaseString isEqualToString:@"i"] &&
          modifiers == (NSEventModifierFlagControl | NSEventModifierFlagShift)) {
        if (!event.isARepeat) show_inspector_help(true);
        return nil;
      }
      return event;
    }];
    if (delegate_->options.url.empty()) {
      auto url = [NSURL fileURLWithPath:ns(delegate_->options.entry.u8string())];
      [webview_ loadFileURL:url allowingReadAccessToURL:[url URLByDeletingLastPathComponent]];
    } else [webview_ loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:ns(delegate_->options.url)]]];
  }
  ~MacWindow() override {
    if (mouse_monitor_) [NSEvent removeMonitor:mouse_monitor_];
    if (key_monitor_) [NSEvent removeMonitor:key_monitor_];
    [inspector_help_.parentWindow removeChildWindow:inspector_help_];
    [inspector_help_ close];
    webview_->sourceClosed = true; webview_->dropEnabled = false; webview_->receiveDrop = {};
    auto drag_reply = std::move(webview_->dragReply);
    if (drag_reply) try { drag_reply({{"result", false}}); } catch (...) {}
    delegate_->isClosed = true;
    [webview_ stopLoading];
    webview_.navigationDelegate = nil;
    webview_.UIDelegate = nil;
    [webview_.configuration.userContentController removeScriptMessageHandlerForName:@"reaweb"];
    [webview_ removeFromSuperview];
    window_.reset();
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
  void devtools() override {
    show_inspector_help();
    throw Error("INSPECTOR_MENU", "macOS: enable Safari Settings > Advanced > Show features for web developers, then choose Develop > this Mac > REAPER > the tool page.");
  }
  void show_inspector_help(bool toggle = false) {
    if (toggle && inspector_help_.visible) { [inspector_help_ orderOut:nil]; return; }
    if (!inspector_help_) {
      inspector_help_ = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 560, 240)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
      inspector_help_.title = @"ReaWebAPI — Safari Web Inspector Setup";
      inspector_help_.releasedWhenClosed = NO;
      auto text = [NSTextField wrappingLabelWithString:@"Inspect this page with Safari Web Inspector.\n\n1. In Safari, enable Settings > Advanced > Show features for web developers.\n2. Select Develop > this Mac > REAPER > the tool page.\n\nCtrl+Shift+I toggles this guide. Open and close the inspector in Safari."];
      text.frame = NSMakeRect(20, 20, 520, 200);
      [inspector_help_.contentView addSubview:text];
      [inspector_help_ center];
    }
    if (inspector_help_.parentWindow != webview_.window) {
      [inspector_help_.parentWindow removeChildWindow:inspector_help_];
      [webview_.window addChildWindow:inspector_help_ ordered:NSWindowAbove];
    }
    [inspector_help_ makeKeyAndOrderFront:nil];
  }
  Json devtools_state() const override { return devtools_prefs_.state(); }
  void restore_devtools(const Json& value) override { devtools_prefs_.restore(value); devtools_prefs_.floating = true; }
  bool closed() const override { return delegate_->isClosed || window_->closed(); }
  void* native_handle() const override { return window_->handle(); }
  void prepare_dock() override {
    maximized_ = webview_.window.zoomed;
    if (maximized_) [webview_.window zoom:nil];
    window_->prepare_dock();
  }
  void restore_floating() override {
    window_->restore_floating();
    if (maximized_ && !webview_.window.zoomed) [webview_.window zoom:nil];
  }
  void focus() override { window_->focus(); [webview_.window makeFirstResponder:webview_]; }
  void set_title(const std::string& title) override { window_->set_title(title); }
  void set_visible(bool visible) override { window_->set_visible(visible); }
  Json bounds() const override { return window_->placement(); }
  void reload() override { [webview_ reload]; }
  bool visible() const override { return window_->visible() && !webview_.hiddenOrHasHiddenAncestor && !webview_.window.miniaturized; }
  bool focused() const override {
    auto responder = webview_.window.firstResponder;
    return webview_.window.keyWindow && [responder isKindOfClass:[NSView class]] &&
      [(NSView*)responder isDescendantOf:webview_];
  }
  Json placement() const override {
    if (!webview_.window.zoomed) normal_ = window_->placement();
    auto value = webview_.window.zoomed && !normal_.is_null() ? normal_ : window_->placement();
    if (!value.is_null()) value["maximized"] = webview_.window.zoomed != NO;
    return value;
  }
  void restore_placement(const Json& value) override {
    normal_ = value;
    window_->restore_placement(value);
    if (value.value("maximized", false) && !webview_.window.zoomed) [webview_.window zoom:nil];
  }
  Json diagnostics() const override {
    auto inspector = devtools_prefs_.state();
    inspector.update({{"embeddedSupported", false}, {"nativeToggleSupported", false}, {"fallbackReason", "Use Safari Develop > this Mac > REAPER > the tool page. Ctrl+Shift+I toggles the setup guide"}});
    return {{"backend", "WKWebView"}, {"browserVersion", [[[NSBundle bundleForClass:[WKWebView class]] objectForInfoDictionaryKey:@"CFBundleVersion"] UTF8String] ?: "system"}, {"devtools", inspector}};
  }
};
class MacPlatform final : public Platform {
  WKWebsiteDataStore* data_;
  WKProcessPool* pool_;
public:
  explicit MacPlatform(const fs::path& path) {
    // Public WebKit APIs select a persistent profile; WebKit owns its physical storage location.
    const auto profile = path / "wk-profile-id";
    std::string identifier;
    std::ifstream input(profile);
    std::getline(input, identifier);
    NSUUID* uuid = identifier.empty() ? nil : [[NSUUID alloc] initWithUUIDString:ns(identifier)];
    if (!uuid && fs::exists(profile)) throw std::runtime_error("Invalid WKWebView App profile id; browser data was left unchanged");
    if (!uuid) {
      uuid = [NSUUID UUID];
      std::ofstream output(profile, std::ios::trunc);
      output << uuid.UUIDString.UTF8String;
      output.close();
      if (!output) throw std::runtime_error("Cannot persist the WKWebView App profile id");
    }
    data_ = [WKWebsiteDataStore dataStoreForIdentifier:uuid];
    pool_ = [WKProcessPool new];
  }
  void desktop(const std::string& method, const Json& args, DesktopReply reply) override {
    @autoreleasepool {
      if (method == "ReaWeb_RevealPath") {
        auto url = [NSURL fileURLWithPath:ns(args.at(0).get<std::string>())];
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[url]];
        reply({{"result", true}}); return;
      }
      if (method == "ReaWeb_OpenExternal") {
        const auto url = args[0].get<std::string>(); validate_external_url(url);
        if (![[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:ns(url)]]) throw Error("EXTERNAL_OPEN_FAILED", "The system could not open this link");
        reply({{"result", true}}); return;
      }
      auto pasteboard = [NSPasteboard generalPasteboard];
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
    @autoreleasepool { return std::make_shared<MacWindow>(std::move(options), data_, pool_); }
  }
};
}
std::unique_ptr<Platform> make_platform(const fs::path& data) { return std::make_unique<MacPlatform>(data); }
}
