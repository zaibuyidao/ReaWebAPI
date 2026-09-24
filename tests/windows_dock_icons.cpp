// Opt-in integration test loaded beside ReaWebAPI in a disposable REAPER profile.
#include <reaper_plugin.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
reaper_plugin_info_t* host;
std::filesystem::path resource, page;
int (*open_window)(const char*, const char*, const char*, const bool*);
bool (*is_open)(int), (*is_docked)(int), (*set_docked)(int, bool), (*send)(int, const char*);
const char* (*receive)(int);
void (*add_dock)(HWND, const char*, const char*, bool), (*activate_dock)(HWND);
HWND window, second, sibling, container, focus, foreground;
LRESULT original_small, original_large, main_small, main_large;
LONG_PTR original_frame;
RECT bounds{};
int id, other_id, phase;
bool ready, other_ready, floating;
ULONGLONG started;
constexpr LONG_PTR frame_mask = WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME;

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
LRESULT icon(HWND hwnd, int size) { return SendMessageW(hwnd, WM_GETICON, size, 0); }
unsigned color(HWND hwnd, int size = ICON_SMALL) {
  const auto image = reinterpret_cast<HICON>(icon(hwnd, size));
  if (!image) return 0;
  ICONINFO info{}; require(GetIconInfo(image, &info), "Invalid native icon handle");
  BITMAP bitmap{}; GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap);
  BITMAPINFO format{}; format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  format.bmiHeader.biWidth = bitmap.bmWidth; format.bmiHeader.biHeight = -bitmap.bmHeight;
  format.bmiHeader.biPlanes = 1; format.bmiHeader.biBitCount = 32; format.bmiHeader.biCompression = BI_RGB;
  std::vector<unsigned> pixels(bitmap.bmWidth * bitmap.bmHeight);
  auto dc = GetDC(nullptr);
  const auto rows = GetDIBits(dc, info.hbmColor, 0, bitmap.bmHeight, pixels.data(), &format, DIB_RGB_COLORS);
  ReleaseDC(nullptr, dc); DeleteObject(info.hbmColor); DeleteObject(info.hbmMask);
  require(rows != 0, "Cannot read native icon pixels");
  return pixels[pixels.size() / 2 + bitmap.bmWidth / 2] & 0xffffff;
}
void snapshot() {
  focus = GetFocus(); foreground = GetForegroundWindow(); GetWindowRect(container, &bounds);
}
void unchanged() {
  RECT current{}; GetWindowRect(container, &current);
  require(EqualRect(&bounds, &current), "Icon update moved/resized the Docker");
  require(GetFocus() == focus && GetForegroundWindow() == foreground, "Icon update changed focus");
}
bool restored() {
  return icon(container, ICON_SMALL) == original_small && icon(container, ICON_BIG) == original_large &&
    (GetWindowLongPtrW(container, GWL_EXSTYLE) & frame_mask) == original_frame;
}
bool matches(HWND child, unsigned expected) {
  if (color(child) != expected || color(child, ICON_BIG) != expected) return false;
  if (!floating || !is_docked(child == window ? id : other_id)) return true;
  if (color(container) != expected || color(container, ICON_BIG) != expected) return false;
  const auto frame = GetWindowLongPtrW(container, GWL_EXSTYLE);
  return expected ? !(frame & frame_mask) : (frame & WS_EX_DLGMODALFRAME) != 0;
}
BOOL CALLBACK find_window(HWND hwnd, LPARAM target) {
  wchar_t cls[128]{}, title[128]{};
  GetClassNameW(hwnd, cls, 128); GetWindowTextW(hwnd, title, 128);
  if (std::wstring(cls) == L"ReaWebAPI.Window" && std::wstring(title) == reinterpret_cast<const wchar_t*>(target)) second = hwnd;
  return TRUE;
}
HWND find(const wchar_t* title) {
  second = nullptr;
  EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM target) -> BOOL {
    find_window(hwnd, target); EnumChildWindows(hwnd, find_window, target); return TRUE;
  }, reinterpret_cast<LPARAM>(title));
  require(second != nullptr, "Native WebView window missing"); return second;
}
void advance() {
  std::ofstream(resource / "dock-icon-test.log", std::ios::app) << "phase=" << phase++ << " passed\n";
  started = GetTickCount64(); snapshot();
}
void request(const char* message) { require(send(id, message), "Could not send icon command"); }
void tick();
void finish(const std::string& result) {
  std::ofstream(resource / "dock-icon-test.log", std::ios::app) << result << '\n';
  host->Register("-timer", reinterpret_cast<void*>(tick));
  PostMessageW(host->hwnd_main, WM_CLOSE, 0, 0);
}
void report(int handle, bool& is_ready) {
  if (!handle || !is_open(handle)) return;
  const std::string result = receive(handle);
  require(result.rfind("ERROR:", 0) != 0, result.c_str());
  if (result == "ready") is_ready = true;
}
void tick() {
  try {
    require(GetTickCount64() - started < 15000, ("Timeout in icon phase " + std::to_string(phase)).c_str());
    if (!open_window) {
      open_window = reinterpret_cast<decltype(open_window)>(host->GetFunc("ReaWeb_Open"));
      if (!open_window) return;
      is_open = reinterpret_cast<decltype(is_open)>(host->GetFunc("ReaWeb_IsOpen"));
      is_docked = reinterpret_cast<decltype(is_docked)>(host->GetFunc("ReaWeb_IsDocked"));
      set_docked = reinterpret_cast<decltype(set_docked)>(host->GetFunc("ReaWeb_SetDocked"));
      send = reinterpret_cast<decltype(send)>(host->GetFunc("ReaWeb_Send"));
      receive = reinterpret_cast<decltype(receive)>(host->GetFunc("ReaWeb_Receive"));
      sibling = CreateWindowW(L"STATIC", L"Other tab", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, host->hwnd_main, nullptr, nullptr, nullptr);
      add_dock(sibling, nullptr, "dock-icon-other", true); activate_dock(sibling);
      container = GetAncestor(sibling, GA_ROOT); floating = container != host->hwnd_main;
      original_small = icon(container, ICON_SMALL); original_large = icon(container, ICON_BIG);
      original_frame = GetWindowLongPtrW(container, GWL_EXSTYLE) & frame_mask;
      main_small = icon(host->hwnd_main, ICON_SMALL); main_large = icon(host->hwnd_main, ICON_BIG);
      id = open_window(page.u8string().c_str(), "dock-icon-a", nullptr, nullptr);
      require(id != 0, "ReaWeb_Open failed");
      window = find(L"ReaWebAPI — dock-icon-test"); second = nullptr;
      if (is_docked(id)) require(!set_docked(id, false) && !is_docked(id), "Initial undock failed");
    }
    report(id, ready); report(other_id, other_ready);
    require(icon(host->hwnd_main, ICON_SMALL) == main_small && icon(host->hwnd_main, ICON_BIG) == main_large,
      "Changed REAPER's main window icon");
    switch (phase) {
      case 0:
        if (!ready || !matches(window, 0xff0000)) return;
        require(set_docked(id, true), "Dock failed");
        require(GetParent(window) == GetParent(sibling), "Test tabs must share a Docker"); advance(); break;
      case 1:
        if (!matches(window, 0xff0000)) return;
        if (std::filesystem::exists(resource / "dock-icon-test.pause")) { started = GetTickCount64(); snapshot(); return; }
        unchanged(); request("hide"); advance(); break;
      case 2:
        if (!matches(window, 0)) return;
        unchanged(); require(!set_docked(id, false) && !is_docked(id), "Hidden undock failed"); advance(); break;
      case 3:
        if (!matches(window, 0) || !restored()) return;
        require(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_DLGMODALFRAME, "Hidden floating icon slot returned");
        require(set_docked(id, true), "Hidden dock failed"); advance(); break;
      case 4:
        if (!matches(window, 0)) return;
        request("green.svg"); advance(); break;
      case 5:
        if (GetTickCount64() - started < 250) return;
        require(matches(window, 0), "Replacing a hidden icon made it visible");
        unchanged(); request("show"); advance(); break;
      case 6:
        if (!matches(window, 0x00ff00)) return;
        unchanged(); activate_dock(sibling); advance(); break;
      case 7:
        if (!restored()) return;
        require(!IsWindowVisible(window) && IsWindowVisible(sibling), "Native tab selection changed");
        request("blue.svg"); advance(); break;
      case 8:
        if (color(window) != 0x0000ff) return;
        require(restored(), "Inactive WebView overwrote the Docker icon"); unchanged();
        other_id = open_window(page.u8string().c_str(), "dock-icon-b", nullptr, nullptr);
        require(other_id != 0, "Second WebView failed"); second = find(L"ReaWebAPI — dock-icon-test"); advance(); break;
      case 9:
        if (!other_ready || color(second) != 0xff0000) return;
        require(set_docked(other_id, true), "Second dock failed"); advance(); break;
      case 10:
        if (!matches(second, 0xff0000)) return;
        activate_dock(window); advance(); break;
      case 11:
        if (!matches(window, 0x0000ff)) return;
        activate_dock(second); advance(); break;
      case 12:
        if (!matches(second, 0xff0000)) return;
        SendMessageW(second, WM_COMMAND, IDCANCEL, 0); advance(); break;
      case 13:
        if (is_open(other_id) || IsWindow(second)) return;
        other_id = 0; second = nullptr; activate_dock(window); advance(); break;
      case 14:
        if (!matches(window, 0x0000ff)) return;
        SendMessageW(window, WM_COMMAND, IDCANCEL, 0); advance(); break;
      case 15:
        if (is_open(id) || IsWindow(window) || !restored()) return;
        ready = false; id = open_window(page.u8string().c_str(), "dock-icon-a", nullptr, nullptr);
        require(id && is_docked(id), "Docking was not restored after close");
        window = find(L"ReaWebAPI — dock-icon-test"); second = nullptr; advance(); break;
      case 16:
        if (!ready || !matches(window, 0xff0000)) return;
        request("clear"); advance(); break;
      case 17:
        if (icon(window, ICON_SMALL) || icon(window, ICON_BIG) || !restored()) return;
        unchanged();
        finish("PASS: favicon, Dock/Undock, visibility, hidden replacement, inactive tab, two WebViews, close/reopen, clear, host icons, focus and bounds"); break;
    }
  } catch (const std::exception& error) { finish(std::string("FAIL: ") + error.what()); }
}
}

extern "C" __declspec(dllexport) int ReaperPluginEntry(HINSTANCE, reaper_plugin_info_t* rec) {
  if (!rec) return 0;
  host = rec;
  resource = std::filesystem::u8path(reinterpret_cast<const char*(*)()>(host->GetFunc("GetResourcePath"))());
  if (!std::filesystem::exists(resource / "dock-icon-test.enabled")) return 0;
  page = resource / "Scripts" / "dock-icon-test" / "index.html";
  std::filesystem::create_directories(page.parent_path());
  for (const auto& name : {"red", "green", "blue"}) {
    const char* fill = std::string(name) == "green" ? "#00ff00" : name;
    std::ofstream(page.parent_path() / (std::string(name) + ".svg"))
      << "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'><rect width='32' height='32' fill='" << fill << "'/></svg>";
  }
  std::ofstream(page) << R"(<title>Dock icon test</title><link rel="icon" href="red.svg"><h1>Dock icon test</h1><script>
(async () => {
  await reaper.lifecycle.ready;
  await reaper.events.on('message', async message => {
    try {
      if (message === 'hide' || message === 'show') await reaper.window.setIconVisible(message === 'show');
      else if (message === 'clear') document.querySelector('link').remove();
      else await reaper.window.setIcon(message);
    } catch (e) { await reaper.host.send('ERROR:' + e); }
  });
  await reaper.host.send('ready');
})().catch(e => reaper.host.send('ERROR:' + e));
</script>)";
  add_dock = reinterpret_cast<decltype(add_dock)>(host->GetFunc("DockWindowAddEx"));
  activate_dock = reinterpret_cast<decltype(activate_dock)>(host->GetFunc("DockWindowActivate"));
  reinterpret_cast<void(*)(const char*, int)>(host->GetFunc("Dock_UpdateDockID"))("dock-icon-other", 0);
  std::ofstream(resource / "dock-icon-test.log") << "REAPER Docker icon integration test\n";
  started = GetTickCount64(); host->Register("timer", reinterpret_cast<void*>(tick));
  return 1;
}
