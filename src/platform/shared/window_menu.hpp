#pragma once
#include "platform/platform.hpp"
#include "platform/shared/devtools.hpp"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <swell/swell.h>
#endif

namespace reaweb {
inline HMENU create_window_menu(const WindowOptions& options, DevToolsMenuState state, bool find_enabled = true, bool folder_enabled = true, bool page_menu = false) {
  auto menu = CreatePopupMenu();
  if (!menu) return nullptr;
  auto append = [&](int id, const std::string& label, bool enabled = true, bool checked = false, const char* shortcut = nullptr) {
    std::string text;
    for (char c : label) {
      if (c == '&') text += '&';
      text += c == '\t' || c == '\r' || c == '\n' ? ' ' : c;
    }
    if (shortcut) { text += '\t'; text += shortcut; }
    const auto flags = MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED) | (checked ? MF_CHECKED : MF_UNCHECKED);
#ifdef _WIN32
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    AppendMenuW(menu, flags, id, wide.c_str());
#else
    InsertMenu(menu, -1, MF_BYPOSITION | flags, id, text.c_str());
#endif
  };
  const auto name = options.app_name ? options.app_name() : options.title;
  if (!page_menu || options.on_dock_toggle) append(1, "Dock " + name + " in Docker", !!options.on_dock_toggle, options.is_docked && options.is_docked());
  append(2, "Reload", true, false, "Ctrl+R");
  append(7, "Find", find_enabled, false, "Ctrl+F");
#ifdef __APPLE__
  const char* inspector_key = "Ctrl+Alt+I"; // SWELL maps Ctrl to Command on macOS.
#else
  const char* inspector_key = "Ctrl+Shift+I";
#endif
  append(3, state.shown ? "Hide DevTools" : "Open DevTools", state.available, false, inspector_key);
  append(4, state.floating ? "Embed DevTools" : "Float DevTools", state.mode_enabled());
  append(5, "Open HTML Folder", folder_enabled);
  append(6, "Close " + name);
  return menu;
}
inline void dispatch_window_menu(int command, const WindowOptions& options, DevToolsMenuState state,
    const std::function<void(DevToolsAction)>& devtools, const std::function<void()>& reload,
    const std::function<void()>& find, const std::function<void()>& folder) {
  if (command == 1 && options.on_dock_toggle) options.on_dock_toggle();
  else if (command == 2) { if (!options.on_reload || !options.on_reload()) reload(); }
  else if (command == 3 && state.available) devtools(state.shown ? DevToolsAction::Hide : DevToolsAction::Open);
  else if (command == 4 && state.mode_enabled()) devtools(state.floating ? DevToolsAction::Embed : DevToolsAction::Float);
  else if (command == 7 && find) find();
  else if (command == 5 && folder) folder();
  else if (command == 6 && options.on_close) options.on_close();
}
// REAPER forwards Docker tab context menus to the docked window.
inline void show_window_menu(HWND owner, LPARAM position, const WindowOptions& options,
    DevToolsMenuState state, std::function<void(DevToolsAction)> devtools, std::function<void()> reload,
    std::function<void()> find, std::function<void()> folder, bool page_menu = false) {
  auto menu = create_window_menu(options, state, !!find, !!folder, page_menu);
  if (!menu) return;
  POINT point{static_cast<short>(LOWORD(position)), static_cast<short>(HIWORD(position))};
  if (position == -1) GetCursorPos(&point);
  const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
    point.x, point.y, 0, owner, nullptr);
  DestroyMenu(menu);
  dispatch_window_menu(command, options, state, devtools, reload, find, folder);
}
}
