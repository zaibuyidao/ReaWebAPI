#include <reaper_plugin.h>
#include "reaweb_stream.h"
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <thread>
#include <string>

namespace {
ReaWeb_ServiceHandle service = 0;
ReaWeb_StreamHandle video = 0;
ReaWeb_CreateFrameStreamFn create_frame;
ReaWeb_PublishFrameFn publish_frame;
ReaWeb_CloseStreamFn close_stream;
ReaWeb_UnregisterServiceFn unregister_service;
ReaWeb_CompleteServiceCallFn complete;
std::thread producer;
std::atomic<bool> stopping{false};
std::atomic<uint64_t> count{0}, longest_publish_us{0};
using Clock = std::chrono::steady_clock;
void stop() {
  stopping = true;
  if (producer.joinable()) producer.join();
  if (video) close_stream(video);
  video = 0;
}
int request(void*, uint64_t handle, uint64_t id, int, const char* method, const char*) {
  if (!std::strcmp(method, "start")) {
    stop();
    ReaWeb_StreamDesc desc{}; desc.size = sizeof(desc); desc.abi_version = 1;
    desc.format = REAWEB_PIXEL_RGBA8; desc.width = 240; desc.height = 160; desc.stride = 960;
    desc.max_bytes = 240 * 160 * 4; desc.capacity = 3; desc.update_rate = 60; desc.owner = service;
    auto status = create_frame("test.video", &desc, &video); if (status) return status;
    stopping = false; count = longest_publish_us = 0;
    producer = std::thread([] {
      std::array<unsigned char, 240 * 160 * 4> pixels{};
      auto next = Clock::now();
      while (!stopping) {
        const auto sequence = ++count;
        for (size_t i = 0; i < pixels.size(); i += 4) { pixels[i] = static_cast<unsigned char>(sequence); pixels[i + 1] = 64; pixels[i + 2] = 128; pixels[i + 3] = 255; }
        const auto begin = Clock::now();
        publish_frame(video, pixels.data(), pixels.size(), sequence, std::chrono::duration<double>(begin.time_since_epoch()).count());
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count();
        if (uint64_t(duration) > longest_publish_us) longest_publish_us = duration;
        next += std::chrono::microseconds(16667); std::this_thread::sleep_until(next);
      }
    });
  } else if (!std::strcmp(method, "stop")) stop();
  else if (std::strcmp(method, "stats")) return REAWEB_METHOD_NOT_FOUND;
  const auto json = "{\"frames\":" + std::to_string(count.load()) + ",\"maxPublishUs\":" + std::to_string(longest_publish_us.load()) + "}";
  return id ? complete(handle, id, json.c_str(), REAWEB_OK, nullptr) : REAWEB_OK;
}
}
extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t* info) {
  if (!info) { stop(); if (service) unregister_service(service); service = 0; return 0; }
  if (!info->GetFunc) return 0;
  const auto add = reinterpret_cast<ReaWeb_RegisterServiceFn>(info->GetFunc("ReaWeb_RegisterService"));
  const auto shutdown = reinterpret_cast<ReaWeb_SetServiceShutdownFn>(info->GetFunc("ReaWeb_SetServiceShutdown"));
  create_frame = reinterpret_cast<ReaWeb_CreateFrameStreamFn>(info->GetFunc("ReaWeb_CreateFrameStream"));
  publish_frame = reinterpret_cast<ReaWeb_PublishFrameFn>(info->GetFunc("ReaWeb_PublishFrame"));
  close_stream = reinterpret_cast<ReaWeb_CloseStreamFn>(info->GetFunc("ReaWeb_CloseStream"));
  unregister_service = reinterpret_cast<ReaWeb_UnregisterServiceFn>(info->GetFunc("ReaWeb_UnregisterService"));
  complete = reinterpret_cast<ReaWeb_CompleteServiceCallFn>(info->GetFunc("ReaWeb_CompleteServiceCall"));
  if (!add || !shutdown || !create_frame || !publish_frame || !close_stream || !unregister_service || !complete) return 0;
  ReaWeb_ServiceCallbacks callbacks{sizeof(callbacks), 1, nullptr, request, nullptr};
  if (add("stream.test", &callbacks, &service)) return 0;
  shutdown(service, [](void*) { stop(); service = 0; });
  return 1;
}
