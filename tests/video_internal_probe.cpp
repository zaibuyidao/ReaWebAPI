// Isolated REAPER experiment. Not linked into ReaWebAPI or installed by packaging.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <string>
#include <chrono>
#include <reaper_plugin.h>
#include <wdltypes.h>
#include <video_frame.h>
#include <video_processor.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <link.h>
#endif

namespace {
constexpr int rgba = 0x52474241, yv12 = 0x59563132;
#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
constexpr size_t pullOffset = 0x1af30c, visibleOffset = 0x26c14c, windowOffset = 0x26bd70;
constexpr size_t displayOffset = 0x1a8848, frameOffset = 0x26bdb0, processedOffset = 0x26bda8;
constexpr uint64_t fingerprint = 0x1c464285c9172a4bull;
#elif defined(__APPLE__) && defined(__aarch64__)
constexpr size_t pullOffset = 0x15408, visibleOffset = 0x110860, windowOffset = 0x110858;
constexpr size_t displayOffset = 0x13868, frameOffset = 0x110ac8, processedOffset = 0x110ad0;
constexpr uint64_t fingerprint = 0xf7f9fd8bb05eb080ull;
#elif defined(__linux__) && defined(__x86_64__)
constexpr size_t pullOffset = 0x133220, visibleOffset = 0x3f8148, windowOffset = 0x3f8150;
constexpr size_t displayOffset = 0x1382f0, frameOffset = 0x3f7e78, processedOffset = 0x3f7e70;
constexpr uint64_t fingerprint = 0xa6ef4de40c14ed0cull;
#else
#error This experiment has no verified offsets for this architecture
#endif
reaper_plugin_info_t* host;
const char* (*getState)(const char*, const char*);
void (*setState)(const char*, const char*, const char*, bool);
FILE* logFile;
std::string last, modulePath;
unsigned char* video;
bool ready, forced;
unsigned char savedVisible;
double lastRequest;

double seconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint64_t hashBytes(uint64_t h, const unsigned char* p, size_t n) {
  while (n--) { h ^= *p++; h *= 1099511628211ull; }
  return h;
}
bool verifyModule() {
#ifdef _WIN32
  video = reinterpret_cast<unsigned char*>(GetModuleHandleW(L"reaper_video.dll"));
  wchar_t path[32768]{};
  if (!video || !GetModuleFileNameW(reinterpret_cast<HMODULE>(video), path, 32768)) return false;
  auto file = _wfopen(path, L"rb");
#elif defined(__APPLE__)
  for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
    const char* name = _dyld_get_image_name(i);
    if (name && std::strstr(name, "/reaper_video.dylib")) {
      modulePath = name;
      video = reinterpret_cast<unsigned char*>(const_cast<mach_header*>(_dyld_get_image_header(i)));
    }
  }
  auto file = video ? std::fopen(modulePath.c_str(), "rb") : nullptr;
#else
  dl_iterate_phdr([](dl_phdr_info* info, size_t, void*) {
    if (std::strstr(info->dlpi_name, "/reaper_video.so")) {
      modulePath = info->dlpi_name; video = reinterpret_cast<unsigned char*>(info->dlpi_addr); return 1;
    }
    return 0;
  }, nullptr);
  auto file = video ? std::fopen(modulePath.c_str(), "rb") : nullptr;
#endif
  if (!file) return false;
  uint64_t hash = 14695981039346656037ull;
  unsigned char buffer[16384];
  while (const auto n = std::fread(buffer, 1, sizeof(buffer), file)) hash = hashBytes(hash, buffer, n);
  const bool valid = !std::ferror(file) && hash == fingerprint;
  std::fclose(file);
  return valid;
}
bool windowExists() {
  uintptr_t window = 0;
  std::memcpy(&window, video + windowOffset, sizeof(window));
  return window != 0;
}
void restore() {
  if (forced) {
    // The experiment only owns the visibility byte while no native window exists.
    if (!windowExists()) video[visibleOffset] = savedVisible;
    forced = false;
  }
}
void result(const char* text) {
  if (logFile) { std::fprintf(logFile, "%s\n", text); std::fflush(logFile); }
  setState("VideoInternal", "result", text, false);
}
void tick() {
  if (forced && seconds() - lastRequest > 3) restore();
  const char* command = getState("VideoInternal", "request");
  if (!command || !*command || last == command) return;
  last = command;
  int id = 0, force = 0;
  double time = 0;
  if (std::sscanf(command, "%d %lf %d", &id, &time, &force) != 3 || !std::isfinite(time)) {
    result("{\"error\":\"invalid request\"}"); return;
  }
  if (!ready) {
    if (!verifyModule()) { result("{\"error\":\"unsupported or missing video module\"}"); return; }
    ready = true;
    for (auto name : {"video_CreateVideoProcessor", "video_CreateDecoders", "videoEnumSourcesNearTime",
                      "videoGetSourcePlaybackInfo2", "Video_MutexLock", "ReaplaySetVideoWindow"}) {
      if (logFile) std::fprintf(logFile, "API %s %p\n", name, host->GetFunc(name));
    }
    auto create = reinterpret_cast<IREAPERVideoProcessor*(*)(void*, int)>(host->GetFunc("video_CreateVideoProcessor"));
    auto processor = create ? create(nullptr, 0x100) : nullptr;
    if (logFile) std::fprintf(logFile, "null context %s\n", processor ? "accepted" : "rejected");
    delete processor;
  }
  lastRequest = seconds();
  if (force && windowExists()) { restore(); result("{\"error\":\"close native video window before forcing\"}"); return; }
  if (force && !forced) { savedVisible = video[visibleOffset]; video[visibleOffset] = 1; forced = true; }
  if (!force) restore();
  auto pull = reinterpret_cast<IVideoFrame*(*)(double, bool)>(video + pullOffset);
  const double started = seconds();
  IVideoFrame* frame = nullptr;
  if (force == 2) {
    reinterpret_cast<void(*)()>(video + displayOffset)();
    std::memcpy(&frame, video + processedOffset, sizeof(frame));
    if (!frame) std::memcpy(&frame, video + frameOffset, sizeof(frame));
    if (frame) frame->AddRef();
  } else frame = pull(time, false);
  const double pullMs = (seconds() - started) * 1000;
  char text[1024];
  if (!frame) {
    std::snprintf(text, sizeof(text), "{\"id\":%d,\"frame\":false,\"nativeWindow\":%s,\"visibleFlag\":%u,\"pullMs\":%.3f}",
                  id, windowExists() ? "true" : "false", video[visibleOffset], pullMs);
  } else {
    struct Release { IVideoFrame* frame; ~Release() { frame->Release(); } } release{frame};
    const auto bits = reinterpret_cast<const unsigned char*>(frame->get_bits());
    const int w = frame->get_w(), h = frame->get_h(), fmt = frame->get_fmt(), stride = frame->get_rowspan();
    if (!bits || w < 12 || h < 12 || w > 8192 || h > 8192 || (fmt != rgba && fmt != yv12) ||
        stride < w * (fmt == rgba ? 4 : 1) || stride > 65536) {
      result("{\"error\":\"unsupported frame layout\"}"); return;
    }
    uint64_t hash = 14695981039346656037ull;
    for (int y = 0; y < h; ++y) hash = hashBytes(hash, bits + size_t(y) * stride, w * (fmt == rgba ? 4 : 1));
    const size_t mid = size_t(h / 2) * stride + (w / 2) * (fmt == rgba ? 4 : 1);
    const size_t corner = size_t(10) * stride + 10 * (fmt == rgba ? 4 : 1);
    std::snprintf(text, sizeof(text),
      "{\"id\":%d,\"frame\":true,\"time\":%.6f,\"end\":%.6f,\"w\":%d,\"h\":%d,\"fmt\":%d,\"stride\":%d,"
      "\"hash\":\"%llu\",\"pixel\":[%u,%u,%u],\"corner\":[%u,%u,%u],\"nativeWindow\":%s,\"visibleFlag\":%u,\"pullMs\":%.3f}",
      id, frame->time_start, frame->time_end, w, h, fmt, stride, static_cast<unsigned long long>(hash),
      bits[mid], bits[mid + 1], bits[mid + 2], bits[corner], bits[corner + 1], bits[corner + 2],
      windowExists() ? "true" : "false", video[visibleOffset], pullMs);
  }
  result(text);
}
}
extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t* rec) {
  if (!rec) {
    if (host) host->Register("-timer", reinterpret_cast<void*>(tick));
    restore();
    if (logFile) std::fclose(logFile);
    return 0;
  }
  const char* optIn = std::getenv("REAWEB_VIDEO_INTERNAL_PROBE");
  if (!optIn || std::strcmp(optIn, "1") || rec->caller_version != REAPER_PLUGIN_VERSION) return 0;
  host = rec;
  getState = reinterpret_cast<decltype(getState)>(rec->GetFunc("GetExtState"));
  setState = reinterpret_cast<decltype(setState)>(rec->GetFunc("SetExtState"));
  auto resource = reinterpret_cast<const char*(*)()>(rec->GetFunc("GetResourcePath"));
  if (!getState || !setState || !resource) return 0;
  logFile = std::fopen((std::string(resource()) + "/probe.log").c_str(), "w");
  return rec->Register("timer", reinterpret_cast<void*>(tick)) ? 1 : 0;
}
