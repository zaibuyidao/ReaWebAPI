# DevTools

**English** | [简体中文](devtools.zh-CN.md)

On Windows and Linux, press **Ctrl+Shift+I** in the WebView or its managed DevTools window to show or hide DevTools. Holding the keys does not repeat the toggle. Hiding returns focus to the page. `reaper.debug.openDevTools()` and `ReaWeb_DevTools(id)` request opening or showing DevTools. Repeated API calls do not hide it.

| Platform | Presentation | Mode switching |
| --- | --- | --- |
| Linux / WebKitGTK | Right-hand panel by default, with a draggable divider | **Float DevTools** / **Dock right** in the panel toolbar |
| Windows / WebView2 | Native floating DevTools window | The current implementation supports floating mode only |
| macOS / WKWebView | Safari Web Inspector | Ctrl+Shift+I toggles the setup guide. Control the inspector in Safari |

## Linux

The panel defaults to 40% of the available width. Drag the divider to adjust it between 20% and 80%. Resizing the main window preserves the ratio. **Float DevTools** / **Dock right** moves the same inspector view between containers, retaining the Console and Inspector session without reloading the page.

Ctrl+Shift+I also works inside the inspector. The shortcut, **Hide DevTools**, and the floating window's close button hide the panel while retaining the session. The inspector's own close control can end the session and reset inspector state on reopening.

Floating DevTools is non-modal and associated with the current REAPER parent through X11. Stacking depends on the window manager. X11/XWayland is required.

## Windows

The Windows backend uses WebView2's [`OpenDevToolsWindow`](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2#opendevtoolswindow). Embedded mode and float/dock switching are not implemented.

When ReaWebAPI identifies the native window, the shortcut also works while DevTools has focus and hides/shows the same window while preserving the session. The native close button ends the session. If the window cannot be identified, use its close button. This can occur when DevTools was opened through the native **Inspect** context menu. If keyboard handling in DevTools is unavailable, use the shortcut in the main WebView. Diagnostics report these limitations in `devtools.lastError`.

The identified window is non-modal and owned by the current REAPER root window. It is not globally always-on-top. Moving focus to DevTools does not suspend or reload the main WebView.

## macOS

ReaWebAPI makes the WKWebView page available to Safari Web Inspector:

1. Enable **Safari Settings > Advanced > Show features for web developers**.
2. Choose **Develop > this Mac > REAPER > the tool page**.

Ctrl+Shift+I toggles the same non-modal guide from either the page or the guide. Hiding the guide returns focus to the page. `reaper.debug.openDevTools()` shows the guide and rejects with `INSPECTOR_MENU`. Open and close the inspector in Safari. ReaWebAPI does not manage its layout or window stacking.

## Saved preferences and diagnostics

`ReaWebAPI/WindowState/*.json` saves `devtools.mode` (`embedded` or `floating`) and `devtools.widthRatio` per page and window slot. Preferences survive reopening the tool and restarting REAPER. DevTools starts closed. Debugging sessions are not persisted. Windows and macOS restore floating mode and retain the width preference.

`(await reaper.debug.getDiagnostics()).devtools` reports the effective mode, width ratio, embedding support and backend-specific status. Safari inspector visibility is unavailable. On Windows, visibility refers to the native window identified by ReaWebAPI. See [manual verification](SMOKE_TEST.md#devtools-acceptance).
