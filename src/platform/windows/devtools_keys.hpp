#pragma once
#include <windows.h>
#include <future>
#include <memory>
#include <thread>

namespace reaweb {
// DevTools runs in Chromium's process and does not forward its keys to the controller.
class DevToolsKeys {
  inline static std::weak_ptr<DevToolsKeys> shared_;
  std::thread thread_;
  DWORD thread_id_ = 0;
  bool available_ = false;

  static LRESULT CALLBACK keyboard(int code, WPARAM message, LPARAM parameter) {
    static thread_local bool down = (GetAsyncKeyState('I') & 0x8000) != 0;
    static thread_local bool consumed = false;
    if (code == HC_ACTION) {
      const auto& key = *reinterpret_cast<KBDLLHOOKSTRUCT*>(parameter);
      if (key.vkCode == 'I') {
        if (message == WM_KEYUP || message == WM_SYSKEYUP) {
          down = false;
          if (consumed) { consumed = false; return 1; }
        } else if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
          const bool repeat = down; down = true;
          if (consumed) return 1;
          GUITHREADINFO gui{sizeof(gui)};
          auto focus = GetGUIThreadInfo(0, &gui) ? gui.hwndFocus : nullptr;
          HWND target = nullptr;
          // Embedded Chromium is a child of the host, including inside REAPER's docker.
          for (auto window = focus; window && !target; window = GetAncestor(window, GA_PARENT))
            target = reinterpret_cast<HWND>(GetPropW(window, target_property));
          if (!target) target = reinterpret_cast<HWND>(GetPropW(GetForegroundWindow(), target_property));
          DWORD process = 0;
          if (target) GetWindowThreadProcessId(target, &process);
          if (process == GetCurrentProcessId() &&
              (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
              !(GetAsyncKeyState(VK_MENU) & 0x8000) &&
              !(GetAsyncKeyState(VK_LWIN) & 0x8000) && !(GetAsyncKeyState(VK_RWIN) & 0x8000)) {
            consumed = true;
            if (!repeat) PostMessageW(target, toggle_message, 0, 0);
            return 1;
          }
        }
      }
    }
    return CallNextHookEx(nullptr, code, message, parameter);
  }
public:
  static constexpr UINT toggle_message = WM_APP + 73;
  static constexpr const wchar_t* target_property = L"ReaWebAPI.DevTools.ToggleTarget";
  DevToolsKeys() {
    std::promise<bool> ready;
    auto result = ready.get_future();
    thread_ = std::thread([this, &ready] {
      thread_id_ = GetCurrentThreadId();
      MSG message{}; PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
      HMODULE module = nullptr;
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&keyboard), &module);
      auto hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard, module, 0);
      ready.set_value(hook != nullptr);
      if (!hook) return;
      // Keep the hook responsive even while REAPER's UI thread is busy.
      while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
      }
      UnhookWindowsHookEx(hook);
    });
    available_ = result.get();
  }
  ~DevToolsKeys() {
    if (available_) PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    if (thread_.joinable()) thread_.join();
  }
  static std::shared_ptr<DevToolsKeys> acquire() {
    auto keys = shared_.lock();
    if (!keys) { keys = std::make_shared<DevToolsKeys>(); shared_ = keys; }
    return keys;
  }
  bool available() const { return available_; }
};
}
