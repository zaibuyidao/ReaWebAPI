// Opt-in observation of REAPER's final-frame cache, not compositor presentation.
#include "core/core.hpp"
#include "video/fingerprint.hpp"
#include "video/scheduler.hpp"
#define WDL_ALLOW_UNSIGNED_DEFAULT_CHAR
#include <reaper_plugin.h>
#include <wdltypes.h>
#include <video_frame.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <link.h>
#endif

namespace {
namespace fs = std::filesystem;
#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
constexpr const char* fingerprint = "b50bc6486f996acc2731acc6889217fdfb8f55dd8e6524746e7d7582ff705f59";
constexpr size_t processed_offset = 0x26bda8, frame_offset = 0x26bdb0;
#elif defined(__APPLE__) && defined(__aarch64__)
constexpr const char* fingerprint = "318fef1c8d990840587b26cc58e2b7e9659844156b8578ba540e493c09f32ba0";
constexpr size_t processed_offset = 0x110ad0, frame_offset = 0x110ac8;
#elif defined(__linux__) && defined(__x86_64__)
constexpr const char* fingerprint = "29efc78f2099a22a5a5c3cd31c0136f31af0d14ad619901fd7d0e914a2a7f3fd";
constexpr size_t processed_offset = 0x3f7e70, frame_offset = 0x3f7e78;
#else
#error No cadence probe offsets for this platform
#endif
struct Module { unsigned char* base = nullptr; fs::path path; };
Module find_module() {
  Module found;
#ifdef _WIN32
  const auto handle = GetModuleHandleW(L"reaper_video.dll");
  wchar_t path[32768]{};
  if (handle && GetModuleFileNameW(handle, path, 32768)) {
    found.base = reinterpret_cast<unsigned char*>(handle); found.path = path;
  }
#elif defined(__APPLE__)
  for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
    const char* path = _dyld_get_image_name(i);
    if (path && fs::path(path).filename() == "reaper_video.dylib") {
      found.base = reinterpret_cast<unsigned char*>(const_cast<mach_header*>(_dyld_get_image_header(i)));
      found.path = fs::u8path(path); break;
    }
  }
#else
  dl_iterate_phdr([](dl_phdr_info* image, size_t, void* context) {
    if (image->dlpi_name && fs::path(image->dlpi_name).filename() == "reaper_video.so") {
      auto& found = *static_cast<Module*>(context);
      found.base = reinterpret_cast<unsigned char*>(image->dlpi_addr); found.path = fs::u8path(image->dlpi_name); return 1;
    }
    return 0;
  }, &found);
#endif
  return found;
}
std::unique_ptr<reaweb::VideoScheduler> scheduler;
const char* (*get_state)(const char*, const char*);
void (*set_state)(const char*, const char*, const char*, bool);
double (*clock_seconds)();
Module module;
bool active = false, has_frame = false, prepared = false;
double begin = 0, last_time = 0;
double previous_tick = 0, max_tick_gap = 0;
reaweb::Json gaps = reaweb::Json::array();
uint64_t samples = 0, changes = 0;
std::set<int> ids;
std::string error;

bool verify_module() {
  module = find_module();
  if (!module.base) return false;
  std::ifstream input(module.path, std::ios::binary | std::ios::ate);
  if (!input) return false;
  const auto length = input.tellg();
  if (length <= 0 || length > 64 * 1024 * 1024) return false;
  std::vector<unsigned char> bytes(static_cast<size_t>(length));
  input.seekg(0); input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  return input && reaweb::video_module_fingerprint(std::move(bytes)) == fingerprint;
}
void tick() {
  const auto value = get_state("CadenceProbe", "request");
  const std::string command = value ? value : "";
  if (command == "prepare") {
    active = false;
    prepared = verify_module();
    set_state("CadenceProbe", "prepared", prepared ? "1" : "0", false);
    set_state("CadenceProbe", "request", "", false);
  } else if (command == "start") {
    set_state("CadenceProbe", "request", "", false);
    set_state("CadenceProbe", "result", "", false);
    begin = clock_seconds(); samples = changes = 0; has_frame = false; ids.clear(); error.clear();
    previous_tick = max_tick_gap = 0; gaps = reaweb::Json::array();
    const auto loaded = find_module();
    active = prepared && loaded.base == module.base && loaded.path == module.path;
    if (!active) error = "VIDEO_HOST_UNSUPPORTED";
  } else if (command == "stop") {
    active = false;
    const double elapsed = std::max(0.0, clock_seconds() - begin);
    reaweb::Json result{{"seconds", elapsed}, {"samples", samples}, {"changedFrames", changes},
      {"uniqueIds", ids.size()}, {"frameIds", ids}, {"frameFPS", elapsed > 0 ? changes / elapsed : 0},
      {"maxTickGapMs", max_tick_gap * 1000}, {"tickGaps", gaps}};
    if (!error.empty()) result["error"] = error;
    set_state("CadenceProbe", "result", result.dump().c_str(), false);
    set_state("CadenceProbe", "request", "", false);
  }
  if (!active) return;
  const double now = clock_seconds();
  if (previous_tick) {
    const double gap = now - previous_tick;
    max_tick_gap = std::max(max_tick_gap, gap);
    if (gap > 0.025 && gaps.size() < 1024) gaps.push_back({now - begin, gap * 1000});
  }
  previous_tick = now;
  const auto loaded = find_module();
  if (loaded.base != module.base || loaded.path != module.path) {
    active = false; error = "VIDEO_MODULE_CHANGED"; return;
  }
  ++samples;
  IVideoFrame* frame = nullptr;
  std::memcpy(&frame, module.base + processed_offset, sizeof(frame));
  if (!frame) std::memcpy(&frame, module.base + frame_offset, sizeof(frame));
  if (!frame || !std::isfinite(frame->time_start) || (has_frame && frame->time_start == last_time)) return;
  last_time = frame->time_start; has_frame = true; ++changes;
  if (frame->get_w() != 1280 || frame->get_h() != 720) return;
  const int format = frame->get_fmt(), stride = frame->get_rowspan();
  const int bytes_per_pixel = format == 0x52474241 ? 4 : format == 0x59563132 ? 1 : format == 0x59555932 ? 2 : 0;
  const auto* bits = reinterpret_cast<const unsigned char*>(frame->get_bits());
  if (!bits || !bytes_per_pixel || stride < 1280 * bytes_per_pixel || stride > 262144) return;
  int id = 0;
  for (int i = 0; i < 12; ++i)
    if (bits[24 * stride + (24 + i * 20) * bytes_per_pixel] > 128) id |= 1 << i;
  ids.insert(id);
}
}

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t* rec) {
  if (!rec) {
    if (scheduler) { scheduler->stop(); scheduler.reset(); }
    active = false;
    if (set_state) set_state("CadenceProbe", "available", "", false);
    return 0;
  }
  const char* opt_in = std::getenv("REAWEB_CADENCE_PROBE");
  if (!opt_in || std::strcmp(opt_in, "1") || rec->caller_version != REAPER_PLUGIN_VERSION) return 0;
  get_state = reinterpret_cast<decltype(get_state)>(rec->GetFunc("GetExtState"));
  set_state = reinterpret_cast<decltype(set_state)>(rec->GetFunc("SetExtState"));
  clock_seconds = reinterpret_cast<decltype(clock_seconds)>(rec->GetFunc("time_precise"));
  if (!get_state || !set_state || !clock_seconds) return 0;
  try {
    scheduler = reaweb::make_video_scheduler(); scheduler->start(tick);
    set_state("CadenceProbe", "available", "1", false);
    return 1;
  } catch (...) { scheduler.reset(); return 0; }
}
