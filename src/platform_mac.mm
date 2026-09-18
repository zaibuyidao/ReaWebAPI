#include "platform.hpp"
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include "swell_window.hpp"
#include <fstream>

@interface ReaWebDelegate : NSObject <WKScriptMessageHandler, WKNavigationDelegate, WKUIDelegate> {
@public
  reaweb::WindowOptions options;
  std::string entryURI;
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
  (void)webView;
  bool allowed = action.targetFrame && action.targetFrame.mainFrame &&
    reaweb::same_document(action.request.URL.absoluteString.UTF8String ?: "", entryURI);
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

namespace reaweb {
namespace {
NSString* ns(const std::string& text) { return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding]; }
class MacWindow final : public Window {
  std::unique_ptr<SwellWindow> window_;
  WKWebView* webview_;
  ReaWebDelegate* delegate_;
  mutable Json normal_;
  bool maximized_ = false;
public:
  MacWindow(WindowOptions options, WKWebsiteDataStore* data, WKProcessPool* pool) {
    delegate_ = [ReaWebDelegate new];
    delegate_->options = std::move(options);
    delegate_->entryURI = file_uri(delegate_->options.entry);
    delegate_->isClosed = false;
    auto config = [WKWebViewConfiguration new];
    config.websiteDataStore = data;
    config.processPool = pool;
    [config.userContentController addScriptMessageHandler:delegate_ name:@"reaweb"];
    auto script = [[WKUserScript alloc] initWithSource:ns(delegate_->options.script)
      injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES];
    [config.userContentController addUserScript:script];
    webview_ = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 860, 640) configuration:config];
    webview_.navigationDelegate = delegate_;
    webview_.UIDelegate = delegate_;
    webview_.inspectable = YES;
    webview_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    window_ = std::make_unique<SwellWindow>(delegate_->options.title, delegate_->options.parent);
    auto content = (__bridge NSView*)GetDlgItem(static_cast<HWND>(window_->handle()), 0);
    webview_.frame = content.bounds;
    [content addSubview:webview_];
    auto url = [NSURL fileURLWithPath:ns(delegate_->options.entry.u8string())];
    [webview_ loadFileURL:url allowingReadAccessToURL:[url URLByDeletingLastPathComponent]];
  }
  ~MacWindow() override {
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
  void devtools() override {
    throw Error("INSPECTOR_MENU", "macOS: enable Safari Settings > Advanced > Show features for web developers, then choose Develop > this Mac > REAPER > the tool page.");
  }
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
    return {{"backend", "WKWebView"}, {"browserVersion", [[[NSBundle bundleForClass:[WKWebView class]] objectForInfoDictionaryKey:@"CFBundleVersion"] UTF8String] ?: "system"}};
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
    if (!uuid) {
      uuid = [NSUUID UUID];
      std::ofstream output(profile, std::ios::trunc);
      output << uuid.UUIDString.UTF8String;
      if (!output) throw std::runtime_error("Cannot persist the shared WKWebView profile id");
    }
    data_ = [WKWebsiteDataStore dataStoreForIdentifier:uuid];
    pool_ = [WKProcessPool new];
  }
  std::shared_ptr<Window> open(WindowOptions options) override {
    @autoreleasepool { return std::make_shared<MacWindow>(std::move(options), data_, pool_); }
  }
};
}
std::unique_ptr<Platform> make_platform(const fs::path& data) { return std::make_unique<MacPlatform>(data); }
}
