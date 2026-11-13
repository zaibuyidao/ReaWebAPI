#include "platform/macos/mac_resources.hpp"
#include "bridge_script.hpp"
#include <fstream>
#include <iostream>
#include <httplib.h>

@interface IsolationMessages : NSObject <WKScriptMessageHandler>
@property(nonatomic, strong) NSMutableArray<NSString*>* values;
@end
@implementation IsolationMessages
- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
  (void)controller;
  if ([message.body isKindOfClass:NSString.class]) [self.values addObject:message.body];
}
@end

int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc != 3) return 2;
    using namespace reaweb;
    try {
      const fs::path source = argv[1], output = argv[2];
      fs::create_directories(output);
      fs::copy_file(source / "tests/webkit_isolation_probe.cjs", output / "probe.js", fs::copy_options::overwrite_existing);
      [NSApplication sharedApplication];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
      [NSApp finishLaunching];
      Json reports = Json::array();
      for (int phase = 0; phase < 6; ++phase) {
        std::ofstream(output / "index.html") << "<!doctype html><script>webkit.messageHandlers.reaweb.postMessage('APP_EXECUTED')</script><script src='probe.js'></script>";
        auto resources = std::make_shared<WebResources>(output, "webkit-isolation-acceptance", phase % 3 != 0);
        httplib::Server server;
        server.Get("/.*", [resources](const httplib::Request& request, httplib::Response& response) {
          const auto result = resources->request(resources->origin() + request.path, "GET");
          response.status = result.status;
          for (const auto& [name, value] : result.headers) response.set_header(name, value);
          if (result.data) response.body.assign(result.data, result.size);
        });
        int port = phase >= 3 ? server.bind_to_any_port("127.0.0.1") : -1;
        std::thread listener;
        if (phase >= 3) listener = std::thread([&] { server.listen_after_bind(); });
        auto handler = [ReaWebSchemeHandler new]; handler->resources = resources;
        auto config = [WKWebViewConfiguration new];
        config.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
        [config setURLSchemeHandler:handler forURLScheme:@"reaweb"];
        auto messages = [IsolationMessages new]; messages.values = [NSMutableArray new];
        [config.userContentController addScriptMessageHandler:messages name:@"reaweb"];
        if (phase % 3 == 2) {
          const auto script = "(() => { const reawebIsolationRequired = true;" + bridge_script + "})();";
          [config.userContentController addUserScript:[[WKUserScript alloc] initWithSource:[NSString stringWithUTF8String:script.c_str()]
            injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];
        }
        auto view = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 640, 480) configuration:config];
        auto window = [[NSWindow alloc] initWithContentRect:NSMakeRect(20, 20, 640, 480)
          styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        window.releasedWhenClosed = NO; window.contentView = view; [window orderFront:nil];
        const auto entry = phase < 3 ? resources->origin() + "/index.html" : "http://127.0.0.1:" + std::to_string(port) + "/index.html";
        [view loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:[NSString stringWithUTF8String:entry.c_str()]]]];
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        while (std::chrono::steady_clock::now() < deadline) {
          auto event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:.02]
            inMode:NSDefaultRunLoopMode dequeue:YES];
          if (event) [NSApp sendEvent:event];
        }
        Json row = {{"phase", phase}, {"headers", resources->request(resources->origin() + "/index.html", "GET").headers}, {"messages", Json::array()}};
        for (NSString* value in messages.values) row["messages"].push_back(value.UTF8String);
        reports.push_back(row);
        [view stopLoading]; [config.userContentController removeScriptMessageHandlerForName:@"reaweb"];
        window.contentView = nil; [window close];
        if (listener.joinable()) { server.stop(); listener.join(); }
      }
      std::ofstream(output / "report.json") << reports.dump(2);
      std::cout << reports.dump(2) << '\n';
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
}
