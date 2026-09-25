# DevTools

**English** | [简体中文](devtools.zh-CN.md)

Press **Ctrl+Shift+I** on Windows/Linux or **Option+Command+I** on macOS in the WebView or its managed DevTools window to show or hide DevTools. Holding the keys does not repeat the toggle. Hiding returns focus to the page. `reaper.debug.openDevTools()` and `ReaWeb_DevTools(id)` request opening or showing DevTools. Repeated API calls do not hide it.

| Platform | Presentation | Mode switching |
| --- | --- | --- |
| Linux / WebKitGTK | Right-hand panel by default, with a draggable divider | **Float DevTools** / **Dock right** in the panel toolbar |
| Windows / WebView2 | Right-hand panel by default, with a draggable divider | **Float DevTools** / **Embed DevTools** in the page context menu |
| macOS / WKWebView | Right-hand panel by default, with a draggable divider | **Float DevTools** / **Embed DevTools** in the page context menu |

## Linux

The panel defaults to 40% of the available width. Drag the divider to adjust it between 20% and 80%. Resizing the main window preserves the ratio. **Float DevTools** / **Dock right** moves the same inspector view between containers, retaining the Console and Inspector session without reloading the page.

Ctrl+Shift+I also works inside the inspector. The shortcut, **Hide DevTools**, and the floating window's close button hide the panel while retaining the session. The inspector's own close control can end the session and reset inspector state on reopening.

Floating DevTools is non-modal and associated with the current REAPER parent through X11. Stacking depends on the window manager. X11/XWayland is required.

## Windows

**Embedded** fills the right-hand panel with the WebView2 inspector content, hiding its title bar, window controls and frame without reserving space. There is no draggable caption. The panel defaults to 40% of the available width, adjustable between 20% and 80% with a 1px divider in the system window-frame color. Resizing preserves the ratio. **Floating** restores the inspector's native title bar, controls and frame without an additional host window. Mode switches reuse the same inspector window, retaining Console and Inspector state without reloading the page.

Right-click the WebView page to access DevTools alongside **Dock in REAPER** / **Undock from REAPER**. The menu shows **Open DevTools** or **Hide DevTools** according to visibility, and **Float DevTools** when embedded or **Embed DevTools** when floating. Changing mode while hidden saves the preference without opening the inspector. The inspector fills its container without a host toolbar, and the Demo has no separate DevTools button.

The menu, shortcut and APIs share one state manager. Ctrl+Shift+I and **Hide DevTools** hide the inspector and retain its session. The floating window's native close button ends the inspector session. Floating DevTools follows the current REAPER root owner and is raised without taking focus when another REAPER window becomes active. It is not globally always-on-top. Focusing DevTools does not suspend or reload the page. On Windows, embedded DevTools supports switching between docked WebViews, including when REAPER and WebView2 use different DPI contexts.

WebView2 exposes no public embedded-inspector controller. ReaWebAPI uses [`OpenDevToolsWindow`](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2#opendevtoolswindow) to create the inspector, identifies its native window, and clips Chromium's custom caption and frame to the measured renderer origin and client edges in Embedded mode. Per-Monitor V1 and V2 hosts use a compatible DPI container without changing REAPER or Chromium's process DPI mode. Unavailable content bounds, unsupported DPI combinations or hosting failures retain the native floating window and disable **Embed DevTools**. `devtools.fallbackReason` explains the fallback. If identification or inspector keyboard handling fails, `devtools.lastError` reports the limitation. An unidentified window must be closed with its own close button. This integration depends on the WebView2 native window implementation.

## macOS

**Embedded** places the native Inspector content view beside the page without a window title bar, window buttons, wrapper frame or reserved decoration space. **Floating** returns the same view to WebKit's own Inspector window. No additional ReaWebAPI window or toolbar is created.

The page context menu shows **Open DevTools** / **Hide DevTools** and **Float DevTools** / **Embed DevTools** according to the current state. **Option+Command+I** toggles visibility from the page or Inspector. Changing mode while hidden does not open it. The shortcut, menu, `reaper.debug.openDevTools()` and `ReaWeb_DevTools(id)` use the same controller. Hiding and switching modes retain the live Inspector view and connection, including Console entries and the selected tab. Native close controls end the session.

The panel defaults to 40% of the available width. Dragging the Inspector divider adjusts the saved ratio between 20% and 80%. Resizing preserves the ratio, including in compact windows. Host dimensions do not disable **Embed DevTools** or force Floating.

Public `WKWebView.inspectable` remains enabled. Programmatic Inspector control and presentation use runtime-checked WebKit private interfaces because the public API has no equivalent controller. AppKit lays out the content view, and Inspector dock controls share the existing mode and width preferences. This integration depends on the system WebKit version. If embedding controls are unavailable, the native floating window remains usable and `embeddedSupported` is `false`. If native control is unavailable, menu actions are disabled, `nativeToggleSupported` is `false`, and `openDevTools()` rejects with `DEVTOOLS_UNAVAILABLE`. Safari's Develop menu remains available for manual inspection. `fallbackReason` and `lastError` describe capability or opening failures.

## Saved preferences and diagnostics

`ReaWebAPI/Apps/<appId>/WindowState/*.json` saves `devtools.mode` (`embedded` or `floating`) and `devtools.widthRatio` per page and window slot. Preferences survive reopening the tool and restarting REAPER. DevTools starts closed. Debugging sessions are not persisted. All platforms restore the saved mode. Windows/macOS hosting fallbacks do not overwrite the saved preference.

`(await reaper.debug.getDiagnostics()).devtools` reports the effective mode, width ratio, embedding support and backend-specific status. On macOS, `visible` reports the local WebKit Inspector state. On Windows, it refers to the native window identified by ReaWebAPI. See [manual verification](SMOKE_TEST.md#devtools-acceptance).
