#pragma once
#include "web/web_resources.hpp"
#include "runtime/video_memory.hpp"
#include <mutex>
#include <atomic>
#import <WebKit/WebKit.h>

namespace reaweb {
struct MacVideoMemory final : VideoMemory {
  mutable std::mutex mutex;
  std::vector<unsigned char> storage;
  mutable std::atomic<uint32_t> slot_bytes[2]{};
  explicit MacVideoMemory(uint32_t size) : storage(size_t(size) * 2) { data = storage.data(); frame_bytes = size; }
  void write_slot(unsigned slot, const void* bytes, uint32_t count = 0) const override {
    std::lock_guard<std::mutex> lock(mutex);
    VideoMemory::write_slot(slot, bytes, count);
    slot_bytes[slot] = count ? count : frame_bytes;
  }
};
}

@interface ReaWebSchemeHandler : NSObject <WKURLSchemeHandler> {
@public
  std::shared_ptr<reaweb::WebResources> resources;
  NSMutableSet* mediaTasks;
  std::map<std::string, std::weak_ptr<reaweb::MacVideoMemory>> videoSlots;
}
@end
@implementation ReaWebSchemeHandler
- (void)readMedia:(std::shared_ptr<reaweb::ResourceResponse>)response task:(id<WKURLSchemeTask>)task position:(uint64_t)position {
  if (![mediaTasks containsObject:task]) return;
  if (position >= response->size) { [mediaTasks removeObject:task]; [task didFinish]; return; }
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    auto bytes = std::make_shared<std::vector<char>>(65536);
    size_t count = 0;
    std::string failure;
    try { count = response->reader->read(response->offset + position, bytes->data(), std::min<size_t>(bytes->size(), response->size - position)); }
    catch (const std::exception& error) { failure = error.what(); }
    dispatch_async(dispatch_get_main_queue(), ^{
      if (![self->mediaTasks containsObject:task]) return;
      if (!failure.empty() || !count) {
        [self->mediaTasks removeObject:task];
        [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCannotOpenFile userInfo:nil]];
        return;
      }
      [task didReceiveData:[NSData dataWithBytes:bytes->data() length:count]];
      [self readMedia:response task:task position:position + count];
    });
  });
}
- (void)webView:(WKWebView*)view startURLSchemeTask:(id<WKURLSchemeTask>)task {
  (void)view;
  if (!resources) {
    [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorResourceUnavailable userInfo:nil]];
    return;
  }
  const std::string uri = task.request.URL.absoluteString.UTF8String ?: "";
  const auto videoPrefix = resources->origin() + "/__reaweb_video__/";
  if (uri.compare(0, videoPrefix.size(), videoPrefix) == 0) {
    const auto key = uri.substr(videoPrefix.size());
    const auto slash = key.find('/');
    const auto found = videoSlots.find(key.substr(0, slash));
    auto memory = found == videoSlots.end() ? nullptr : found->second.lock();
    const bool valid = memory && task.request.HTTPMethod && [task.request.HTTPMethod isEqualToString:@"GET"] &&
      slash != std::string::npos && (key.substr(slash) == "/0" || key.substr(slash) == "/1");
    if (!valid) {
      [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorResourceUnavailable userInfo:nil]];
      return;
    }
    const auto count = memory->slot_bytes[key.back() - '0'].load();
    auto fields = @{@"Content-Type": @"application/octet-stream", @"Cache-Control": @"no-store",
      @"Content-Length": [NSString stringWithFormat:@"%u", count]};
    [task didReceiveResponse:[[NSHTTPURLResponse alloc] initWithURL:task.request.URL statusCode:200 HTTPVersion:@"HTTP/1.1" headerFields:fields]];
    NSData* data;
    {
      std::lock_guard<std::mutex> lock(memory->mutex);
      data = [[NSData alloc] initWithBytesNoCopy:memory->data + (key.back() - '0') * size_t(memory->frame_bytes) length:count
        deallocator:^(void*, NSUInteger) { (void)memory; }];
    }
    [task didReceiveData:data];
    [task didFinish];
    return;
  }
  std::map<std::string, std::string> headers;
  for (NSString* key in task.request.allHTTPHeaderFields)
    headers[key.lowercaseString.UTF8String] = [task.request.allHTTPHeaderFields[key] UTF8String];
  auto response = resources->request(task.request.URL.absoluteString.UTF8String ?: "", task.request.HTTPMethod.UTF8String ?: "GET", headers);
  auto fields = [NSMutableDictionary dictionary];
  for (const auto& [name, value] : response.headers)
    fields[[NSString stringWithUTF8String:name.c_str()]] = [NSString stringWithUTF8String:value.c_str()];
  auto native = [[NSHTTPURLResponse alloc] initWithURL:task.request.URL statusCode:response.status HTTPVersion:@"HTTP/1.1" headerFields:fields];
  [task didReceiveResponse:native];
  if (response.reader) {
    if (!mediaTasks) mediaTasks = [NSMutableSet set];
    [mediaTasks addObject:task];
    [self readMedia:std::make_shared<reaweb::ResourceResponse>(std::move(response)) task:task position:0];
    return;
  }
  if (response.size) {
    const auto owner = response.owner;
    auto data = [[NSData alloc] initWithBytesNoCopy:const_cast<char*>(response.data) length:response.size
      deallocator:^(void*, NSUInteger) { (void)owner; }];
    [task didReceiveData:data];
  }
  [task didFinish];
}
- (void)webView:(WKWebView*)view stopURLSchemeTask:(id<WKURLSchemeTask>)task { (void)view; [mediaTasks removeObject:task]; }
@end
