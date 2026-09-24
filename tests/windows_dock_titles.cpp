// Opt-in integration test loaded by a disposable REAPER instance, alongside ReaWebAPI.
#include <reaper_plugin.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {
reaper_plugin_info_t* host;
std::filesystem::path resource, page;
int (*open_window)(const char*, const char*, const char*, const bool*);
bool (*is_open)(int), (*is_docked)(int), (*set_docked)(int, bool), (*send)(int, const char*);
const char* (*receive)(int);
void (*add_dock)(HWND, const char*, const char*, bool), (*remove_dock)(HWND), (*activate_dock)(HWND);
HWND window, sibling, container, focus, foreground;
RECT bounds{};
std::wstring inactive_caption;
int id, runs, checks;
bool closing;
ULONGLONG started;

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::wstring caption(HWND hwnd) {
  wchar_t text[1024]{};
  GetWindowTextW(hwnd, text, 1024);
  return text;
}
std::wstring wide(const std::string& text) {
  std::wstring result(MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), static_cast<int>(result.size()));
  result.pop_back(); return result;
}
BOOL CALLBACK find_window(HWND hwnd, LPARAM) {
  wchar_t cls[128]{}; GetClassNameW(hwnd, cls, 128);
  if (std::wstring(cls) == L"ReaWebAPI.Window") window = hwnd;
  return TRUE;
}
void tick();
void finish(const std::string& result) {
  std::ofstream(resource / "dock-title-test.log", std::ios::app) << result << '\n';
  host->Register("-timer", reinterpret_cast<void*>(tick));
  PostMessageW(host->hwnd_main, WM_CLOSE, 0, 0);
}
void snapshot() {
  focus = GetFocus(); foreground = GetForegroundWindow(); GetWindowRect(container, &bounds);
}
void tick() {
  try {
    require(GetTickCount64() - started < 60000, "Timed out waiting for WebView title/close");
    if (!open_window) {
      open_window = reinterpret_cast<decltype(open_window)>(host->GetFunc("ReaWeb_Open"));
      if (!open_window) return;
      is_open = reinterpret_cast<decltype(is_open)>(host->GetFunc("ReaWeb_IsOpen"));
      is_docked = reinterpret_cast<decltype(is_docked)>(host->GetFunc("ReaWeb_IsDocked"));
      set_docked = reinterpret_cast<decltype(set_docked)>(host->GetFunc("ReaWeb_SetDocked"));
      send = reinterpret_cast<decltype(send)>(host->GetFunc("ReaWeb_Send"));
      receive = reinterpret_cast<decltype(receive)>(host->GetFunc("ReaWeb_Receive"));
    }
    if (closing) {
      if (is_open(id)) return;
      closing = false; id = 0;
      if (++runs == 2) { finish("PASS: HTML, dynamic Unicode, fallback, explicit override, inactive tab, Docker close/reopen, focus and placement"); return; }
    }
    if (!id) {
      id = open_window(page.u8string().c_str(), "dock-title-test", nullptr, nullptr);
      require(id != 0, "ReaWeb_Open failed");
      if (runs) require(is_docked(id), "Closed docked window did not restore docking");
      checks = 0; window = nullptr;
      EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM) -> BOOL {
        find_window(hwnd, 0); EnumChildWindows(hwnd, find_window, 0); return TRUE;
      }, 0);
      require(window != nullptr, "Native WebView window missing");
    }
    const std::string report = receive(id);
    if (report.empty()) return;
    require(report.rfind("ERROR:", 0) != 0, report.c_str());
    if (report == "CLOSE") {
      require(checks == 5, "Missing title checks");
      require(container != host->hwnd_main, "Expected a floating Docker");
      // Docker tab closure reaches the child through its native cancel command.
      SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
      closing = true; return;
    }
    if (!checks) {
      if (!is_docked(id)) require(set_docked(id, true), "Could not dock WebView");
      container = GetAncestor(window, GA_ROOT);
      require(container != host->hwnd_main, "Configure dockermode0=32771 and dockcompactsingle=1");
      snapshot();
    }
    const auto expected = wide(report);
    require(caption(window) == expected, "Native WebView caption differs from document title");
    if (sibling) {
      require(!IsWindowVisible(window) && IsWindowVisible(sibling), "Title update selected an inactive tab");
      require(caption(container) == inactive_caption, "Inactive tab changed the Docker caption");
    } else {
      const auto expected_container = expected + L" (docked)";
      if (caption(container) != expected_container) {
        char actual[4096]{};
        WideCharToMultiByte(CP_UTF8, 0, caption(container).c_str(), -1, actual, sizeof(actual), nullptr, nullptr);
        bool floating = false;
        reinterpret_cast<int(*)(HWND, bool*)>(host->GetFunc("DockIsChildOfDock"))(window, &floating);
        throw std::runtime_error("Floating Docker caption did not follow: " + report + "; actual=" + actual +
          "; floating=" + std::to_string(floating) + "; visible=" + std::to_string(IsWindowVisible(window)));
      }
    }
    RECT current{}; GetWindowRect(container, &current);
    require(EqualRect(&bounds, &current), "Title update moved/resized the Docker");
    require(GetFocus() == focus && GetForegroundWindow() == foreground, "Title update changed focus");
    std::ofstream(resource / "dock-title-test.log", std::ios::app) << "run=" << runs << " title=" << report << '\n';
    if (++checks == 2) {
      sibling = CreateWindowW(L"STATIC", L"Other tab", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, host->hwnd_main, nullptr, nullptr, nullptr);
      add_dock(sibling, nullptr, "dock-title-test-other", true); activate_dock(sibling);
      require(GetParent(sibling) == GetParent(window), "Other tab must share the test Docker");
      inactive_caption = caption(container);
    } else if (checks == 3) {
      remove_dock(sibling); DestroyWindow(sibling); sibling = nullptr;
    }
    snapshot();
    require(send(id, "next"), "Could not acknowledge title check");
  } catch (const std::exception& error) { finish(std::string("FAIL: ") + error.what()); }
}
}

extern "C" __declspec(dllexport) int ReaperPluginEntry(HINSTANCE, reaper_plugin_info_t* rec) {
  if (!rec) return 0;
  host = rec;
  resource = std::filesystem::u8path(reinterpret_cast<const char*(*)()>(host->GetFunc("GetResourcePath"))());
  if (!std::filesystem::exists(resource / "dock-title-test.enabled")) return 0;
  page = resource / "Scripts" / "dock-title-test" / "index.html";
  std::filesystem::create_directories(page.parent_path());
  std::ofstream(page) << R"(<title>HTML title</title><meta charset="utf-8"><script>
(async () => {
  await reaper.lifecycle.ready;
  let resume;
  await reaper.events.on('message', () => resume?.());
  const check = async expected => {
    for (let n = 0; (await reaper.window.getState()).title !== expected; ++n) {
      if (n > 300) throw new Error('Title timeout: ' + expected);
      await new Promise(r => setTimeout(r, 10));
    }
    const next = new Promise(r => resume = r);
    await reaper.host.send(expected); await next;
  };
  await check('HTML title');
  document.title = 'Dynamic 标题'; await check(document.title);
  document.title = ''; await check('ReaWebAPI — dock-title-test');
  await reaper.window.setTitle('Explicit 标题'); await check('Explicit 标题');
  document.title = 'Ignored'; await check('Explicit 标题');
  await reaper.host.send('CLOSE');
})().catch(e => reaper.host.send('ERROR:' + e));
</script>)";
  add_dock = reinterpret_cast<decltype(add_dock)>(host->GetFunc("DockWindowAddEx"));
  remove_dock = reinterpret_cast<decltype(remove_dock)>(host->GetFunc("DockWindowRemove"));
  activate_dock = reinterpret_cast<decltype(activate_dock)>(host->GetFunc("DockWindowActivate"));
  auto remember = reinterpret_cast<void(*)(const char*, int)>(host->GetFunc("Dock_UpdateDockID"));
  remember("dock-title-test-other", 0);
  std::ofstream(resource / "dock-title-test.log") << "REAPER Docker title integration test\n";
  started = GetTickCount64();
  host->Register("timer", reinterpret_cast<void*>(tick));
  return 1;
}
