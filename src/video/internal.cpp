#include "video/internal.hpp"
#include "video/fingerprint.hpp"
#define WDL_ALLOW_UNSIGNED_DEFAULT_CHAR
#include <wdltypes.h>
#include <video_frame.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <link.h>
#endif

namespace reaweb {
namespace {
constexpr int rgba = 0x52474241;
struct Build {
  const char* checksum;
  size_t display, visible, window, frame, processed, monitor_hash;
  size_t query, source_hash, cached, destroy_sources, frame_index, is_visible, transform;
};
// Internal offsets are selected by the normalized module fingerprint.
#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
constexpr Build builds[]{{"b50bc6486f996acc2731acc6889217fdfb8f55dd8e6524746e7d7582ff705f59",
  0x1a8848, 0x26c14c, 0x26bd70, 0x26bdb0, 0x26bda8, 0x26bda0,
  0x1a9738, 0x1a5fc0, 0x1af738, 0x1ad4dc, 0x26bd98, 0x211810, 0x1a6ffc}};
#elif defined(__APPLE__) && defined(__aarch64__)
constexpr Build builds[]{{"318fef1c8d990840587b26cc58e2b7e9659844156b8578ba540e493c09f32ba0",
  0x13868, 0x110860, 0x110858, 0x110ac8, 0x110ad0, 0x110ad8,
  0x13100, 0x126ac, 0x12a94, 0x12054, 0x110ae0, 0x10fa28, 0x14fdc}};
#elif defined(__linux__) && defined(__x86_64__)
constexpr Build builds[]{{"f34a2e7634400dedfc70757e5daa7861988529e16ddcb2a3a85c5b72841e9535",
  0x1382f0, 0x3f8148, 0x3f8150, 0x3f7e78, 0x3f7e70, 0x3f7e68,
  0x132b30, 0x12b270, 0x12b660, 0x12ad30, 0x3f7e60, 0x3f21c0, 0x12e590},
  {"ef1bb206879f4a73f80550f551b98bb00b9493dd98648421c70c3d6eda3790ee",
  0x138330, 0x3f8148, 0x3f8150, 0x3f7e78, 0x3f7e70, 0x3f7e68,
  0x132b70, 0x12b2b0, 0x12b6a0, 0x12ad70, 0x3f7e60, 0x3f21c0, 0x12e5d0},
  {"29efc78f2099a22a5a5c3cd31c0136f31af0d14ad619901fd7d0e914a2a7f3fd",
  0x138320, 0x3f8148, 0x3f8150, 0x3f7e78, 0x3f7e70, 0x3f7e68,
  0x133d60, 0x12b2a0, 0x12b690, 0x12ad60, 0x3f7e60, 0x3f21c0, 0x12e5c0}};
#else
constexpr Build builds[]{{}};
#endif
struct Module { unsigned char* base = nullptr; fs::path path; };
Module find_module() {
  Module module;
#ifdef _WIN32
  const auto handle = GetModuleHandleW(L"reaper_video.dll");
  wchar_t path[32768]{};
  if (handle && GetModuleFileNameW(handle, path, 32768)) {
    module.base = reinterpret_cast<unsigned char*>(handle); module.path = path;
  }
#elif defined(__APPLE__)
  for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
    const char* path = _dyld_get_image_name(i);
    if (path && fs::path(path).filename() == "reaper_video.dylib") {
      module.base = reinterpret_cast<unsigned char*>(const_cast<mach_header*>(_dyld_get_image_header(i)));
      module.path = fs::u8path(path); break;
    }
  }
#else
  dl_iterate_phdr([](dl_phdr_info* image, size_t, void* context) {
    if (image->dlpi_name && fs::path(image->dlpi_name).filename() == "reaper_video.so") {
      auto& module = *static_cast<Module*>(context);
      module.base = reinterpret_cast<unsigned char*>(image->dlpi_addr); module.path = fs::u8path(image->dlpi_name); return 1;
    }
    return 0;
  }, &module);
#endif
  return module;
}
template<class T> T read_at(const unsigned char* base, size_t offset) {
  T value{}; std::memcpy(&value, base + offset, sizeof(value)); return value;
}
struct Sources {
  void* buffer = nullptr;
  int allocated = 0, used = 0, granularity = 4096, padding = 0;
  bool owned = false, extra = false;
  unsigned char tail[6]{};
};
static_assert(sizeof(Sources) == 32);
struct FrameRef {
  IVideoFrame* frame;
  ~FrameRef() { if (frame) frame->Release(); }
};
class InternalVideo final : public VideoBackend {
  const Host& host;
  std::thread::id main = std::this_thread::get_id();
  Module module;
  const Build* build = nullptr;
  bool verified = false, leased = false;
  bool has_frame = false;
  int64_t last_index = 0;
  uint64_t last_source = 0, last_monitor = 0;
  double last_start = 0, last_end = 0;
  template<class T> T api(const char* name) const {
    return reinterpret_cast<T>(host.native_function ? host.native_function(name) : nullptr);
  }
  template<class T> T internal(size_t offset) const { return reinterpret_cast<T>(module.base + offset); }
public:
  explicit InternalVideo(const Host& h) : host(h) {}
  ~InternalVideo() override { stop(); }
  bool supported() override {
    if (!builds[0].checksum || std::this_thread::get_id() != main) return false;
    if (!api<void*>("EnumProjects") || !api<void*>("TimeMap_curFrameRate")) return false;
    const auto loaded = find_module();
    if (!loaded.base) return false;
    if (verified && loaded.base == module.base && loaded.path == module.path) return true;
    if (loaded.base != module.base) leased = false;
    verified = false; module = loaded;
    std::ifstream file(module.path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto size = file.tellg();
    if (size <= 0 || size > 64 * 1024 * 1024) return false;
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!file) return false;
    const auto fingerprint = video_module_fingerprint(std::move(bytes));
    for (const auto& candidate : builds) if (fingerprint == candidate.checksum) {
      build = &candidate;
      verified = true;
      return true;
    }
    return false;
  }
  void stop() override {
    has_frame = false;
    if (!leased) return;
    if (std::this_thread::get_id() == main && find_module().base == module.base) {
      auto window = read_at<void*>(module.base, build->window);
#ifdef _WIN32
      auto visible = read_at<BOOL(WINAPI*)(HWND)>(module.base, build->is_visible);
      module.base[build->visible] = window && visible && visible(static_cast<HWND>(window));
#else
      auto visible = read_at<bool(*)(void*)>(module.base, build->is_visible);
      module.base[build->visible] = window && visible && visible(window);
#endif
    }
    leased = false;
  }
  VideoRead read(bool discontinuity, const Capture& capture) override {
    if (discontinuity) has_frame = false;
    if (!supported()) { stop(); return VideoRead::unsupported; }
    const auto projects = api<void*(*)(int, char*, int)>("EnumProjects");
    if (projects(0x40000000, nullptr, 0)) { stop(); return VideoRead::rendering; }
    auto project = projects(-1, nullptr, 0);
    const double fps = api<double(*)(void*, bool*)>("TimeMap_curFrameRate")(project, nullptr);
    if (!project || !std::isfinite(fps) || fps <= 0) { stop(); return VideoRead::empty; }
    module.base[build->visible] = 1; leased = true;
    const auto play = api<int(*)()>("GetPlayState");
    const auto selected = [&](bool retained) -> VideoRead {
    Sources sources, monitor;
    const auto destroy = internal<void(*)(void*)>(build->destroy_sources);
    struct Cleanup { Sources& a; Sources& b; void (*destroy)(void*); ~Cleanup() { destroy(&a); destroy(&b); } } cleanup{sources, monitor, destroy};
    int64_t index = -1;
    internal<int(*)(void*, int64_t*, double, void*, void*, void*, int, void*)>(build->query)
      (&sources, &index, fps, &monitor, project, project, 3, nullptr);
    if (sources.used < 0 || monitor.used < 0) return VideoRead::pending;
    if (!sources.used && !monitor.used) return VideoRead::empty;
    if (retained) {
      const auto displayed_index = read_at<int64_t>(module.base, build->frame_index);
      if (displayed_index < 0 || displayed_index > index || index - displayed_index > 1) return VideoRead::pending;
      index = displayed_index;
    }
    auto hash = internal<uint64_t(*)(void*, void*)>(build->source_hash);
    const auto source_key = sources.used ? hash(&sources, project) : 0;
    const auto monitor_key = monitor.used ? hash(&monitor, project) : 0;
    auto base = read_at<IVideoFrame*>(module.base, build->frame);
    FrameRef cached{nullptr};
    if (sources.used) {
      cached.frame = internal<IVideoFrame*(*)(uint64_t, int64_t, bool, int64_t*, void*)>(build->cached)
        (source_key, index, false, nullptr, project);
      if (!cached.frame || cached.frame != base) return VideoRead::pending;
    } else if (base || index != read_at<int64_t>(module.base, build->frame_index)) return VideoRead::pending;
    auto processed = read_at<IVideoFrame*>(module.base, build->processed);
    if (monitor.used && (!processed || monitor_key != read_at<uint64_t>(module.base, build->monitor_hash))) return VideoRead::pending;
    auto frame = processed ? processed : base;
    if (!frame) return VideoRead::pending;
    const double start = frame->time_start, end = frame->time_end;
    if (play && (play() & 1) && has_frame && index == last_index && source_key == last_source && monitor_key == last_monitor &&
        start == last_start && end == last_end) return VideoRead::unchanged;
    // transformFrame consumes this additional reference, including on failure.
    frame->AddRef();
    frame = internal<IVideoFrame*(*)(IVideoFrame*, bool, int, void**, const char*)>(build->transform)
      (frame, false, rgba, nullptr, nullptr);
    FrameRef reference{frame};
    if (!frame) return VideoRead::pending;
    const int w = frame->get_w(), h = frame->get_h(), stride = frame->get_rowspan();
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || stride <= 0 || stride > 262144 || !frame->get_bits()) return VideoRead::empty;
    if (frame->get_fmt() != rgba || stride < int64_t(w) * 4) return VideoRead::pending;
    capture({reinterpret_cast<const unsigned char*>(frame->get_bits()), w, h, stride});
    has_frame = true; last_index = index; last_source = source_key; last_monitor = monitor_key;
    last_start = start; last_end = end;
    return VideoRead::ready;
    };
    // Read an unseen official frame before display() can replace it with the next one.
    if (!discontinuity && has_frame && play && (play() & 1) &&
        read_at<int64_t>(module.base, build->frame_index) != last_index && selected(true) == VideoRead::ready)
      return VideoRead::ready;
    internal<void(*)()>(build->display)();
    return selected(false);
  }
};
}
std::unique_ptr<VideoBackend> make_video_backend(const Host& host) { return std::make_unique<InternalVideo>(host); }
}
