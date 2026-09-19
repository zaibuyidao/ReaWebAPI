#include "runtime/runtime.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <thread>

using namespace reaweb;
void pump(Runtime& runtime, const std::function<bool()>& done) {
  const auto deadline = Clock::now() + std::chrono::seconds(20);
  do {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    runtime.tick();
    if (done()) return;
    Sleep(5);
  } while (Clock::now() < deadline);
  throw std::runtime_error("Native drag test timed out");
}
void mouse(DWORD flags) {
  INPUT event{}; event.type = INPUT_MOUSE; event.mi.dwFlags = flags;
  if (SendInput(1, &event, sizeof(event)) != 1) throw std::runtime_error("Cannot inject test mouse input");
}
Json read(const fs::path& path) { std::ifstream stream(path); return Json::parse(stream); }
int main() {
  SetProcessDPIAware();
  POINT saved{}; GetCursorPos(&saved);
  struct Restore { POINT point; ~Restore() { INPUT event{}; event.type = INPUT_MOUSE; event.mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(1, &event, sizeof(event)); SetCursorPos(point.x, point.y); } } restore{saved};
  try {
    const auto resource = fs::current_path() / ("native-drag-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(Clock::now().time_since_epoch().count()));
    const auto app = resource / "Scripts" / "App";
    fs::create_directories(app);
    const auto audio = app / fs::u8path("sample 空 格.wav"); std::ofstream(audio) << "native file fixture";
    std::ofstream(app / "source.html") << R"HTML(<!doctype html><meta charset="utf-8"><h1>Drag source test</h1><script>
      (async()=>{
        await reaper.lifecycle.ready; await reaper.window.setTitle('ReaWebAPI native source');
        let phase=0;
        window.addEventListener('pointerdown',e=>{
          if(e.button!==0)return;e.preventDefault();const current=phase++;
          const promise=current===0 ? reaper.dragDrop.startFiles(['sample 空 格.wav']) : reaper.dragDrop.startText('native text 你好');
          promise.then(result=>reaper.fs.writeText('result-'+current+'.json',JSON.stringify(result),{overwrite:true})).catch(error=>reaper.fs.writeText('error.json',JSON.stringify({message:error.message,code:error.code}),{overwrite:true}));
        });
        await reaper.fs.writeText('source-ready','yes',{overwrite:true});
      })();</script>)HTML";
    std::ofstream(app / "target.html") << R"HTML(<!doctype html><meta charset="utf-8"><h1>Native drop target</h1><script>
      (async()=>{
        await reaper.lifecycle.ready; await reaper.window.setTitle('ReaWebAPI native target');
        let count=0;await reaper.dragDrop.onDrop(payload=>reaper.fs.writeText('drop-'+count+++'.json',JSON.stringify(payload),{overwrite:true}));
        await reaper.fs.writeText('target-ready','yes',{overwrite:true});
      })();</script>)HTML";
    Host host; int project;
    host.current_project = [&]() -> void* { return &project; };
    host.change_count = [](void*) { return 0; };
    std::vector<std::string> errors;
    Runtime runtime(host, resource, [&](const std::string& error) { errors.push_back(error); });
    const auto first = runtime.open("App/source.html"), second = runtime.open("App/target.html");
    pump(runtime, [&] { return fs::exists(app / "source-ready") && fs::exists(app / "target-ready"); });
    auto source = FindWindowW(nullptr, L"ReaWebAPI native source"), target = FindWindowW(nullptr, L"ReaWebAPI native target");
    if (!source || !target) throw std::runtime_error("Test windows not found");
    SetWindowPos(source, HWND_TOPMOST, 30, 40, 350, 280, SWP_SHOWWINDOW);
    SetWindowPos(target, HWND_TOPMOST, 440, 40, 350, 280, SWP_SHOWWINDOW);
    std::cout << "Native drag test windows ready\n" << std::flush;
    for (int phase = 0; phase < 3; ++phase) {
      POINT from{100, 100}, to{100, 100}; ClientToScreen(source, &from); ClientToScreen(target, &to);
      SetForegroundWindow(source); SetCursorPos(from.x, from.y);
      const auto settled = Clock::now() + std::chrono::milliseconds(300);
      pump(runtime, [&] { return Clock::now() >= settled; });
      if (GetAncestor(WindowFromPoint(from), GA_ROOT) != source) throw std::runtime_error("Source window is not accessible on the input desktop");
      std::thread gesture([=] {
        Sleep(100); mouse(MOUSEEVENTF_LEFTDOWN); Sleep(600);
        if (phase < 2) {
          for (int i = 1; i <= 12; ++i) { SetCursorPos(from.x + (to.x-from.x)*i/12, from.y + (to.y-from.y)*i/12); Sleep(35); }
          Sleep(100);
        } else {
          INPUT key{}; key.type = INPUT_KEYBOARD; key.ki.wVk = VK_ESCAPE; SendInput(1, &key, sizeof(key));
          Sleep(50); key.ki.dwFlags = KEYEVENTF_KEYUP; SendInput(1, &key, sizeof(key));
        }
        mouse(MOUSEEVENTF_LEFTUP);
      });
      try { pump(runtime, [&] { return fs::exists(app / ("result-" + std::to_string(phase) + ".json")) || fs::exists(app / "error.json"); }); }
      catch (...) { gesture.join(); throw; }
      gesture.join();
      std::cout << "Native source phase " << phase << " finished\n" << std::flush;
      if (fs::exists(app / "error.json")) throw std::runtime_error(read(app / "error.json").dump());
      if (read(app / ("result-" + std::to_string(phase) + ".json")) != (phase < 2)) throw std::runtime_error("Unexpected copy/cancel result");
      if (phase < 2) {
        const auto path = app / ("drop-" + std::to_string(phase) + ".json");
        pump(runtime, [&] { return fs::exists(path); });
        const auto drop = read(path);
        if (phase == 0 && drop["files"] != Json::array({fs::canonical(audio).u8string()})) throw std::runtime_error("Native file path mismatch: " + drop.dump());
        if (phase == 1 && drop["text"] != "native text 你好") throw std::runtime_error("Native text mismatch: " + drop.dump());
        if (!drop["x"].is_number() || !drop["y"].is_number()) throw std::runtime_error("Missing drop coordinates");
      }
    }
    runtime.close(first); runtime.close(second); runtime.tick();
    if (!errors.empty()) throw std::runtime_error(errors.front());
    std::cout << "WebView2/OLE native drag: Unicode file/text round trips, real file paths, copy and Escape cancellation passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
