#include "platform/platform.hpp"
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <objc/runtime.h>
#include "platform/shared/swell_window.hpp"
#include "platform/macos/mac_devtools.hpp"
#include <fstream>
#include <cstring>

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
  std::function<void()> toggleDock;
  std::function<bool()> isDocked;
  std::function<NSInteger(NSMenu*, NSInteger)> devtoolsMenu;
}
@end

@implementation ReaWebNativeView
- (void)willOpenMenu:(NSMenu*)menu withEvent:(NSEvent*)event {
  [super willOpenMenu:menu withEvent:event];
  if (sourceClosed || !menu.numberOfItems) return;
  NSInteger index = 0;
  if (toggleDock) {
    auto item = [[NSMenuItem alloc] initWithTitle:isDocked && isDocked() ? @"Undock from REAPER" : @"Dock in REAPER"
      action:@selector(toggleDockFromMenu:) keyEquivalent:@""];
    item.target = self;
    [menu insertItem:item atIndex:index++];
  }
  if (devtoolsMenu) index = devtoolsMenu(menu, index);
  if (index) [menu insertItem:[NSMenuItem separatorItem] atIndex:index];
}
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
namespace {
NSString* ns(const std::string& text) { return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding]; }
char dock_icon_owner;
class MacWindow final : public Window {
  std::unique_ptr<SwellWindow> window_;
  ReaWebNativeView* webview_;
  id mouse_monitor_;
  std::unique_ptr<MacDevTools> devtools_;
  ReaWebDelegate* delegate_;
  mutable Json normal_;
  bool maximized_ = false;
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
    webview_->toggleDock = delegate_->options.on_dock_toggle;
    webview_->isDocked = delegate_->options.is_docked;
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
    webview_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    window_ = std::make_unique<SwellWindow>(delegate_->options.title, delegate_->options.parent, std::function<void()>{}, delegate_->options.on_close);
    auto content = (__bridge NSView*)GetDlgItem(static_cast<HWND>(window_->handle()), 0);
    webview_.frame = content.bounds;
    [content addSubview:webview_];
    devtools_ = std::make_unique<MacDevTools>(webview_, [this] { if (!closed()) focus(); });
    webview_->devtoolsMenu = [this](NSMenu* menu, NSInteger index) { return devtools_->insert_menu(menu, index); };
    if (delegate_->options.url.empty()) {
      auto url = [NSURL fileURLWithPath:ns(delegate_->options.entry.u8string())];
      [webview_ loadFileURL:url allowingReadAccessToURL:[url URLByDeletingLastPathComponent]];
    } else [webview_ loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:ns(delegate_->options.url)]]];
  }
  ~MacWindow() override {
    release_dock_icon();
    if (mouse_monitor_) [NSEvent removeMonitor:mouse_monitor_];
    webview_->devtoolsMenu = {};
    devtools_.reset();
    webview_->sourceClosed = true; webview_->dropEnabled = false; webview_->receiveDrop = {};
    webview_->toggleDock = {}; webview_->isDocked = {};
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
  void devtools() override { devtools_->open(); }
  Json devtools_state() const override { return devtools_->state(); }
  void restore_devtools(const Json& value) override { devtools_->restore(value); }
  bool closed() const override { return delegate_->isClosed || window_->closed(); }
  void* native_handle() const override { return window_->handle(); }
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
    icon_window_ = nil;
    maximized_ = webview_.window.zoomed;
    if (maximized_) [webview_.window zoom:nil];
    window_->prepare_dock();
  }
  void restore_floating() override {
    icon_window_ = nil;
    window_->restore_floating();
    if (maximized_ && !webview_.window.zoomed) [webview_.window zoom:nil];
  }
  void focus() override { window_->focus(); [webview_.window makeFirstResponder:webview_]; }
  void tick() override { if (!closed()) { devtools_->tick(); apply_icon(); } }
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
    return {{"backend", "WKWebView"}, {"browserVersion", [[[NSBundle bundleForClass:[WKWebView class]] objectForInfoDictionaryKey:@"CFBundleVersion"] UTF8String] ?: "system"}, {"devtools", devtools_->diagnostics()}};
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
        const auto url = args[0].get<std::string>(); validate_external_url(url);
        if (![[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:ns(url)]]) throw Error("EXTERNAL_OPEN_FAILED", "The system could not open this link");
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
    @autoreleasepool { return std::make_shared<MacWindow>(std::move(options), data_, pool_); }
  }
};
}
std::unique_ptr<Platform> make_platform(const fs::path& data) { return std::make_unique<MacPlatform>(data); }
}
