// Opt-in acceptance inside an isolated REAPER profile and X11 display.
#include <reaper_plugin.h>
#include "platform/platform.hpp"
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <fstream>
#include <set>

namespace {
using namespace reaweb;
reaper_plugin_info_t* host;
fs::path resource;
Display* display = nullptr;
::Window native = 0;
std::set<::Window> children;
int id = 0, mode = 0, repeat = 0, phase = 0, teardown = 0;
bool hidden = false, destroyed = false, exposed = false;
DWORD started, due;
void tick();
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void log(const std::string& message) { std::ofstream(resource / "close-test.log", std::ios::app) << message << '\n'; }
void watch(::Window window) {
  XSelectInput(display, window, StructureNotifyMask);
  ::Window root, parent, *list = nullptr; unsigned count = 0;
  if (XQueryTree(display, window, &root, &parent, &list, &count)) {
    for (unsigned i = 0; i < count; ++i) { children.insert(list[i]); watch(list[i]); }
    if (list) XFree(list);
  }
}
void finish(const std::string& message) {
  log(message); host->Register("-timer", reinterpret_cast<void*>(tick));
  if (display) { XCloseDisplay(display); display = nullptr; }
  PostMessage(host->hwnd_main, WM_CLOSE, 0, 0);
}
void tick() {
  try {
    require(GetTickCount() - started < 120000, "Close acceptance timed out");
    if (GetTickCount() < due || !IsWindowVisible(host->hwnd_main)) return;
    auto open = reinterpret_cast<int(*)(const char*, const char*)>(host->GetFunc("ReaWeb_OpenEx"));
    auto close = reinterpret_cast<bool(*)(int)>(host->GetFunc("ReaWeb_Close"));
    if (!open) return;
    if (phase == 0) {
      id = open((resource / "Scripts/Close/index.html").c_str(), mode == 1 ? "{}" : mode == 2 ?
        "{\"frameless\":true,\"resizable\":false}" : "{\"frameless\":true}");
      require(id != 0, "Open failed"); phase = 1; due = GetTickCount() + 300; return;
    }
    if (phase == 1) {
      if (!reinterpret_cast<bool(*)(int)>(host->GetFunc("ReaWeb_IsReady"))(id)) return;
      HWND handle = nullptr;
      EnumWindows(+[](HWND window, LPARAM result) -> BOOL {
        char title[128]{}; GetWindowText(window, title, sizeof(title));
        if (std::string(title) == "Close acceptance") { *reinterpret_cast<HWND*>(result) = window; return FALSE; }
        return TRUE;
      }, reinterpret_cast<LPARAM>(&handle));
      if (!handle || !IsWindowVisible(handle)) return;
      auto gdk = SWELL_GetOSWindow(handle, "GdkWindow");
      require(gdk != nullptr, "Missing native X11 host");
      native = reinterpret_cast<unsigned long(*)(void*)>(dlsym(RTLD_DEFAULT, "gdk_x11_window_get_xid"))(gdk);
      children.clear(); watch(native); XSync(display, False);
      XEvent event{}; while (XPending(display)) XNextEvent(display, &event);
      hidden = destroyed = exposed = false; teardown = 0;
      require(!children.empty(), "Missing WebKit child windows");
      if (mode == 2) SendMessage(handle, WM_CLOSE, 0, 0);
      else require(close(id), "Close failed");
      phase = 2; return;
    }
    XSync(display, False);
    while (XPending(display)) {
      XEvent event{}; XNextEvent(display, &event);
      const auto window = event.xany.window;
      if (window == native && event.type == UnmapNotify) hidden = true;
      if (window == native && event.type == DestroyNotify) destroyed = true;
      if (children.count(window) && (event.type == UnmapNotify || event.type == ReparentNotify || event.type == DestroyNotify)) {
        ++teardown; exposed |= !hidden;
      }
    }
    if (!destroyed) return;
    require(hidden, "Host destroyed without unmapping");
    require(teardown > 0, "No WebKit teardown events observed");
    require(!exposed, "WebKit detached before the native host was hidden");
    log("PASS: hide before WebKit teardown, repeat=" + std::to_string(repeat) + " mode=" + std::to_string(mode));
    phase = 0; due = GetTickCount() + 100;
    if (++mode == 3) { mode = 0; if (++repeat == 5) finish("PASS: all close cases"); }
  } catch (const std::exception& error) { finish(std::string("FAIL: ") + error.what()); }
}
}
extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t* rec) {
  if (!rec) return 0;
  host = rec;
  resource = fs::u8path(reinterpret_cast<const char*(*)()>(host->GetFunc("GetResourcePath"))());
  if (!fs::exists(resource / "close-test.enabled")) return 0;
  display = XOpenDisplay(nullptr); if (!display) return 0;
  auto page = resource / "Scripts/Close/index.html"; fs::create_directories(page.parent_path());
  std::ofstream(page.parent_path() / "app.json") << R"({"id":"close-acceptance"})";
  std::ofstream(page) << "<!doctype html><title>Close acceptance</title><style>html{background:#182838}</style><input value=Close>";
  started = GetTickCount(); host->Register("timer", reinterpret_cast<void*>(tick)); return 1;
}
