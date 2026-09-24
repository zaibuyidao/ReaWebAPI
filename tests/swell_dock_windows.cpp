// Opt-in integration test for a disposable macOS/Linux REAPER profile.
#include <reaper_plugin.h>
#include "platform/shared/swell_window.hpp"
#include <fstream>
#include <cmath>
#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#else
#include <X11/Xlib.h>
#include <dlfcn.h>
#endif

namespace {
using namespace reaweb;
reaper_plugin_info_t* host;
fs::path resource, page;
int (*open_window)(const char*, const char*, const char*, const bool*);
bool (*is_open)(int), (*is_docked)(int), (*set_docked)(int, bool), (*send)(int, const char*);
const char* (*receive)(int);
void (*add_dock)(HWND, const char*, const char*, bool), (*activate_dock)(HWND), (*remove_dock)(HWND);
std::unique_ptr<SwellWindow> sibling;
HWND window, second, container, main_window, focus, foreground;
RECT bounds{};
int id, other_id, phase, run;
bool ready, other_ready, acknowledged, floating;
DWORD started;
std::string inactive_caption;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string caption(HWND hwnd) { char text[1024]{}; GetWindowText(hwnd, text, sizeof(text)); return text; }
HWND native_window(HWND hwnd) {
#ifdef __APPLE__
  ::id object = (__bridge ::id)hwnd;
  return (__bridge HWND)([object isKindOfClass:NSWindow.class] ? object : [object window]);
#else
  while (hwnd && !SWELL_GetOSWindow(hwnd, "GdkWindow")) hwnd = GetParent(hwnd);
  return hwnd;
#endif
}
struct IconState {
  std::vector<unsigned long> icon, decorations;
  std::string url;
  unsigned color = 0;
  bool operator==(const IconState& other) const { return icon == other.icon && decorations == other.decorations && url == other.url; }
};
IconState icons(HWND hwnd) {
  IconState result;
#ifdef __APPLE__
  auto native = (__bridge NSWindow*)native_window(hwnd);
  auto image = [native standardWindowButton:NSWindowDocumentIconButton].image;
  result.icon = {reinterpret_cast<unsigned long>((__bridge void*)image)};
  result.url = native.representedURL.absoluteString.UTF8String ?: "";
  for (NSImageRep* rep in image.representations) if ([rep isKindOfClass:NSBitmapImageRep.class]) {
    auto bitmap = (NSBitmapImageRep*)rep;
    auto color = [[bitmap colorAtX:bitmap.pixelsWide / 2 y:bitmap.pixelsHigh / 2] colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
    result.color = (unsigned(std::lround(color.redComponent * 255)) << 16) |
      (unsigned(std::lround(color.greenComponent * 255)) << 8) | unsigned(std::lround(color.blueComponent * 255));
    break;
  }
#else
  auto native = SWELL_GetOSWindow(native_window(hwnd), "GdkWindow");
  auto display_of = reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT, "gdk_window_get_display"));
  auto xdisplay_of = reinterpret_cast<Display*(*)(void*)>(dlsym(RTLD_DEFAULT, "gdk_x11_display_get_xdisplay"));
  auto xid_of = reinterpret_cast<::Window(*)(void*)>(dlsym(RTLD_DEFAULT, "gdk_x11_window_get_xid"));
  require(native && display_of && xdisplay_of && xid_of, "Missing native GDK window");
  auto display = xdisplay_of(display_of(native));
  const auto read = [&](const char* name) {
    Atom type; int format; unsigned long count, remaining; unsigned char* bytes = nullptr;
    require(XGetWindowProperty(display, xid_of(native), XInternAtom(display, name, False), 0, 65536, False,
      AnyPropertyType, &type, &format, &count, &remaining, &bytes) == Success, "Cannot read native icon property");
    std::vector<unsigned long> values;
    if (count && format == 32) values.assign(reinterpret_cast<unsigned long*>(bytes), reinterpret_cast<unsigned long*>(bytes) + count);
    if (bytes) XFree(bytes);
    return values;
  };
  result.icon = read("_NET_WM_ICON"); result.decorations = read("_MOTIF_WM_HINTS");
  if (result.icon.size() > 2) result.color = result.icon[2] & 0xffffff;
#endif
  return result;
}
IconState original, main_icons;
HWND find(const std::string& title) {
  struct Search { const std::string& title; HWND result = nullptr; } search{title};
  EnumWindows(+[](HWND hwnd, LPARAM data) -> BOOL {
    auto& search = *reinterpret_cast<Search*>(data);
    if (caption(hwnd) == search.title) search.result = hwnd;
    EnumChildWindows(hwnd, +[](HWND child, LPARAM data) -> BOOL {
      auto& search = *reinterpret_cast<Search*>(data);
      if (caption(child) == search.title) search.result = child;
      return TRUE;
    }, data);
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  require(search.result != nullptr, "Native WebView window missing"); return search.result;
}
void snapshot() { focus = GetFocus(); foreground = GetForegroundWindow(); GetWindowRect(container, &bounds); }
void unchanged() {
  RECT current{}; GetWindowRect(container, &current);
  require(bounds.left == current.left && bounds.top == current.top && bounds.right == current.right && bounds.bottom == current.bottom,
    "Title/icon update changed Docker bounds");
  require(GetFocus() == focus && GetForegroundWindow() == foreground, "Title/icon update changed focus");
}
void title(const std::string& expected) {
  require(caption(window) == expected, "WebView title mismatch");
  if (floating && phase < 5) require(caption(container).find(expected) != std::string::npos, ("Docker title mismatch: " + caption(container)).c_str());
}
bool matches(HWND child, int handle, unsigned color) {
  return is_docked(handle) && !floating ? true : icons(child).color == color;
}
void request(const char* command) { acknowledged = false; require(send(id, command), "Could not send test command"); }
void advance() {
  std::ofstream(resource / "swell-dock-test.log", std::ios::app) << "run=" << run << " phase=" << phase++ << " passed\n";
  started = GetTickCount(); snapshot();
}
void report(int handle, bool& is_ready) {
  if (!handle || !is_open(handle)) return;
  const std::string value = receive(handle);
  require(value.rfind("ERROR:", 0) != 0, value.c_str());
  if (value == "ready") is_ready = true;
  if (value == "done") acknowledged = true;
}
void tick();
void finish(const std::string& result) {
  std::ofstream(resource / "swell-dock-test.log", std::ios::app) << result << '\n';
  host->Register("-timer", reinterpret_cast<void*>(tick));
  PostMessage(host->hwnd_main, WM_CLOSE, 0, 0);
}
void tick() {
  try {
    require(GetTickCount() - started < 30000, ("Timeout in phase " + std::to_string(phase)).c_str());
    if (!open_window) {
#ifdef __APPLE__
      if (NSApp.modalWindow) {
        bool audio_prompt = false; NSButton* decline = nil;
        std::vector<NSView*> views{NSApp.modalWindow.contentView};
        while (!views.empty()) {
          auto view = views.back(); views.pop_back();
          if ([view isKindOfClass:NSTextField.class] && [[(NSTextField*)view stringValue] containsString:@"You have not yet selected an audio device."]) audio_prompt = true;
          if ([view isKindOfClass:NSButton.class] && [[(NSButton*)view title] isEqualToString:@"No"]) decline = (NSButton*)view;
          for (NSView* child in view.subviews) views.push_back(child);
        }
        if (audio_prompt && decline) [decline performClick:nil];
        return;
      }
#endif
      if (!IsWindowVisible(host->hwnd_main)) return;
      open_window = reinterpret_cast<decltype(open_window)>(host->GetFunc("ReaWeb_Open"));
      if (!open_window) return;
      is_open = reinterpret_cast<decltype(is_open)>(host->GetFunc("ReaWeb_IsOpen"));
      is_docked = reinterpret_cast<decltype(is_docked)>(host->GetFunc("ReaWeb_IsDocked"));
      set_docked = reinterpret_cast<decltype(set_docked)>(host->GetFunc("ReaWeb_SetDocked"));
      send = reinterpret_cast<decltype(send)>(host->GetFunc("ReaWeb_Send"));
      receive = reinterpret_cast<decltype(receive)>(host->GetFunc("ReaWeb_Receive"));
      sibling = std::make_unique<SwellWindow>("Other tab", host->hwnd_main);
      add_dock(static_cast<HWND>(sibling->handle()), nullptr, "swell-dock-other", true);
      activate_dock(static_cast<HWND>(sibling->handle()));
      return;
    }
    if (!main_window) {
      activate_dock(static_cast<HWND>(sibling->handle()));
      container = native_window(static_cast<HWND>(sibling->handle())); main_window = native_window(host->hwnd_main);
      floating = container != main_window; original = icons(container); main_icons = icons(main_window);
      std::string mode; std::ifstream(resource / "swell-dock-test.enabled") >> mode;
      if (!mode.empty()) require(floating == (mode == "floating"), "Docker display mode differs from test configuration");
      std::ofstream(resource / "swell-dock-test.log", std::ios::app) << "mode=" << (floating ? "floating" : "embedded") << '\n';
    }
    if (!id) {
      id = open_window(page.u8string().c_str(), "swell-dock-a", nullptr, nullptr);
      require(id != 0, "Cannot open WebView");
      if (run) require(is_docked(id), "Dock state was not restored after close");
      window = find("ReaWebAPI — swell-dock-test");
    }
    report(id, ready); report(other_id, other_ready);
    require(icons(main_window) == main_icons, "Changed REAPER's main window icon");
    switch (phase) {
      case 0:
        if (!ready || !matches(window, id, 0xff0000)) return;
        require(set_docked(id, true), "Dock failed"); remove_dock(static_cast<HWND>(sibling->handle())); advance(); break;
      case 1:
        if (!matches(window, id, 0xff0000)) return;
        title("HTML title"); request("title"); advance(); break;
      case 2:
        if (!acknowledged) return;
        title("Dynamic 标题"); unchanged(); request("blank"); advance(); break;
      case 3:
        if (!acknowledged) return;
        title("ReaWebAPI — swell-dock-test"); unchanged(); request("explicit"); advance(); break;
      case 4:
        if (!acknowledged) return;
        title("Explicit 标题"); unchanged(); add_dock(static_cast<HWND>(sibling->handle()), nullptr, "swell-dock-other", true);
        activate_dock(static_cast<HWND>(sibling->handle()));
        inactive_caption = caption(container); request("inactive"); advance(); break;
      case 5:
        if (!acknowledged) return;
        require(caption(window) == "Inactive 标题" && !IsWindowVisible(window), "Background title selected the WebView");
        require(caption(container) == inactive_caption && icons(container) == original, "Inactive WebView changed Docker state");
        unchanged(); activate_dock(window); advance(); break;
      case 6:
        if (!matches(window, id, 0xff0000)) return;
        title("Inactive 标题"); request("hide"); advance(); break;
      case 7:
        if (!acknowledged) return;
        require(matches(window, id, 0), "Docked icon did not hide"); unchanged();
        require(!set_docked(id, false) && !is_docked(id), "Undock failed"); advance(); break;
      case 8:
        require(matches(window, id, 0), "Hidden icon returned after undock");
        require(icons(container) == original, "Undock did not restore Docker icon"); request("green.svg"); advance(); break;
      case 9:
        if (!acknowledged) return;
        require(matches(window, id, 0), "Updating hidden icon made it visible");
        require(set_docked(id, true), "Redock failed"); advance(); break;
      case 10:
        if (!matches(window, id, 0)) return;
        request("show"); advance(); break;
      case 11:
        if (!acknowledged) return;
        require(matches(window, id, 0x00ff00), "Current icon was not restored"); unchanged();
        other_id = open_window(page.u8string().c_str(), "swell-dock-b", nullptr, nullptr);
        require(other_id != 0, "Second WebView failed"); second = find("ReaWebAPI — swell-dock-test"); advance(); break;
      case 12:
        if (!other_ready || !matches(second, other_id, 0xff0000)) return;
        require(set_docked(other_id, true), "Second dock failed"); advance(); break;
      case 13:
        if (!matches(second, other_id, 0xff0000)) return;
        activate_dock(window); advance(); break;
      case 14:
        if (!matches(window, id, 0x00ff00)) return;
        activate_dock(second); advance(); break;
      case 15:
        if (!matches(second, other_id, 0xff0000)) return;
        SendMessage(second, WM_COMMAND, IDCANCEL, 0); advance(); break;
      case 16:
        if (is_open(other_id) || IsWindow(second)) return;
        other_id = 0; second = nullptr; other_ready = false; activate_dock(window); advance(); break;
      case 17:
        if (!matches(window, id, 0x00ff00)) return;
        SendMessage(window, WM_COMMAND, IDCANCEL, 0); advance(); break;
      case 18:
        if (is_open(id) || IsWindow(window)) return;
        require(icons(container) == original, "Close did not restore Docker icon");
        if (run++) { finish("PASS: restored docking, HTML/dynamic/fallback/explicit titles, inactive tabs, icons, visibility, two WebViews, close/reopen, host icons, focus and bounds"); return; }
        id = 0; ready = false; phase = 0; started = GetTickCount(); break;
    }
  } catch (const std::exception& error) { finish(std::string("FAIL: ") + error.what()); }
}
}

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t* rec) {
  if (!rec) { sibling.reset(); return 0; }
  host = rec;
  resource = fs::u8path(reinterpret_cast<const char*(*)()>(host->GetFunc("GetResourcePath"))());
  if (!fs::exists(resource / "swell-dock-test.enabled")) return 0;
  page = resource / "Scripts" / "swell-dock-test" / "index.html"; fs::create_directories(page.parent_path());
  for (const auto& name : {"red", "green"}) std::ofstream(page.parent_path() / (std::string(name) + ".svg"))
    << "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'><rect width='32' height='32' fill='"
    << (std::string(name) == "red" ? "#ff0000" : "#00ff00") << "'/></svg>";
  std::ofstream(page) << R"(<meta charset="utf-8"><title>HTML title</title><link rel="icon" href="red.svg"><h1>Dock title and icon test</h1><script>
(async () => {
  await reaper.lifecycle.ready;
  await reaper.events.on('message', async command => {
    try {
      if (command === 'title' || command === 'blank') {
        const expected = command === 'title' ? 'Dynamic 标题' : 'ReaWebAPI — swell-dock-test';
        document.title = command === 'title' ? expected : '';
        for (let n = 0; (await reaper.window.getState()).title !== expected; ++n) {
          if (n > 300) throw Error('Title synchronization timed out');
          await new Promise(resolve => setTimeout(resolve, 10));
        }
      } else if (command === 'explicit' || command === 'inactive') {
        await reaper.window.setTitle(command === 'explicit' ? 'Explicit 标题' : 'Inactive 标题'); document.title = 'Ignored';
      } else if (command === 'hide' || command === 'show') await reaper.window.setIconVisible(command === 'show');
      else await reaper.window.setIcon(command);
      await reaper.host.send('done');
    } catch (error) { await reaper.host.send('ERROR:' + error); }
  });
  await reaper.host.send('ready');
})().catch(error => reaper.host.send('ERROR:' + error));
</script>)";
  add_dock = reinterpret_cast<decltype(add_dock)>(host->GetFunc("DockWindowAddEx"));
  activate_dock = reinterpret_cast<decltype(activate_dock)>(host->GetFunc("DockWindowActivate"));
  remove_dock = reinterpret_cast<decltype(remove_dock)>(host->GetFunc("DockWindowRemove"));
  reinterpret_cast<void(*)(const char*, int)>(host->GetFunc("Dock_UpdateDockID"))("swell-dock-other", 0);
  std::ofstream(resource / "swell-dock-test.log") << "REAPER SWELL Docker integration test\n";
  started = GetTickCount(); host->Register("timer", reinterpret_cast<void*>(tick)); return 1;
}
