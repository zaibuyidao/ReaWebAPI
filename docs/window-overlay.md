# Window overlays and native mouse routing

[简体中文](window-overlay.zh-CN.md)

Overlay policies apply to standalone floating Apps. Embedded JSFX Web UIs retain their existing input and window behavior.

```js
const id = await reaper.window.open('overlay/index.html', {
  frameless: true, transparent: true,
  layer: 'overlay', mouseMode: 'regions',
  mouseRegions: [{x: 20, y: 20, width: 80, height: 80}]
});
// These methods address the calling window.
await reaper.window.setMouseRegions([{x: 20, y: 20, width: 80, height: 80}]);
await reaper.window.setMouseMode('regions');
```

| Option | Default | Contract |
| --- | --- | --- |
| `layer` | `normal` | `overlay` places the window above REAPER's main window and below its floating windows. Creation only, without global topmost status. |
| `mouseMode` | `normal` | `normal` receives client-area input, `passthrough` passes it through, `regions` receives it only inside the region union. Independent of layer and visual transparency. |
| `mouseRegions` | `[]` | Rectangles in viewport CSS pixels. Empty regions pass all client-area input in `regions` mode. |

Each rectangle requires finite `x`, `y`, `width` and `height`. Width and height must be positive. Native viewport clipping handles out-of-bounds portions. Invalid lists reject the entire update. `setMouseRegions` replaces the list atomically. Both setters resolve to `true` on acceptance and reject using the existing error contract. Native mouse gestures retain their original target until release before a pending policy takes effect. System title bars and borders retain native behavior.

The host converts CSS coordinates using native DPI and WebView zoom. Apps submit new rectangles after DOM layout changes, using `getBoundingClientRect()` and layout observers. The host does not infer DOM/SVG shapes. Mouse routing does not change `setKeyboardCapture`, `setDawKeyboard` or existing text/IME rules. Full pass-through does not release keyboard focus previously acquired by the window.

## Lua access

```lua
local id = reaper.ReaWeb_OpenEx(path,
  '{"layer":"overlay","mouseMode":"regions","frameless":true,"transparent":true}')
local ok = reaper.ReaWeb_SetMouseRegions(id, '[{"x":20,"y":20,"width":80,"height":80}]')
ok = reaper.ReaWeb_SetMouseMode(id, 'normal')
if not ok then reaper.ShowConsoleMsg(reaper.ReaWeb_GetLastError() .. '\n') end
-- ReaWeb_Close(id) also works while all mouse input passes through.
```

Lua setters return Boolean and provide diagnostics through `ReaWeb_GetLastError()`. Invalid or closed IDs reject with `WINDOW_CLOSED` in JS and return false in Lua. Instance reuse keeps the original layer, mode and regions and ignores new creation options.

## Lifecycle and capabilities

`getState()` and `windowstatechange` include `layer`, `layerActive`, `mouseMode` and `mouseModeActive`. Active flags describe establishment of the requested native policy, independently of visibility. They can be false during initialization or a deferred mode change. Reload retains layer and mode but clears old document regions. Closing and reopening uses the new creation options. Policies are not persisted across REAPER launches.

Overlay or non-normal mouse policies reject docking with `DOCK_UNAVAILABLE`. Creation temporarily bypasses saved docking without overwriting that preference. Runtime changes on a docked window must first be preceded by explicit undocking. Hosted JSFX editors reject policy setters with `WINDOW_HOSTED`.

Inspect `capabilities.runtime.window.overlay`. Its `layer`, `passthrough`, `regions` and `combined` entries each contain `supported` and `reason`. It also reports `floatingOnly` and `backend`. Capability availability is separate from acceptance-test coverage.

| Platform | Native implementation and conditions |
| --- | --- |
| Windows | Win32 event-driven ordering and pre-dispatch mouse routing for the HWND/WebView2 host. A REAPER main-window owner is required for overlay layering. |
| macOS | AppKit ordering and a session mouse event tap. Non-normal mouse modes require Accessibility permission for REAPER. No permission prompt is opened automatically. |
| Linux | Native X11, X Shape 1.1, XInput 2.1, a window manager supporting `_NET_RESTACK_WINDOW`, and REAPER's GLib loop. Input shapes apply to the host, independent of WebKitGTK rendering. XWayland/Wayland are rejected for these policies. Input-shaped viewports must be smaller than 32768 native pixels per axis. |

`INVALID_ARGUMENT` identifies invalid parameters. `LAYER_UNAVAILABLE` and `MOUSE_MODE_UNAVAILABLE` identify unsupported native conditions. `LAYER_FAILED` and `MOUSE_MODE_FAILED` identify native setup or update failures. A failed runtime update preserves the previous requested state. Asynchronous initialization failures follow existing window cleanup and diagnostics behavior.

The [single-file overlay example](../examples/windows/overlay/README.md) defaults to English and includes a Simplified Chinese selector. Its Window mode enables background dragging and native edge resizing with existing frameless-window support. Input controls are excluded from dragging. The overlay layer remains below floating REAPER windows.

Before using mouse routing on macOS, open **System Settings → Privacy & Security → Accessibility** and enable **REAPER**. Add the application with **+** if it is absent, then quit and reopen REAPER.

Linux frameless overlays may extend beyond the display bounds. The example overscans by one native pixel to avoid window managers treating an exact screen-sized frameless window as fullscreen and raising it above floating windows. Apps fitting a display must retain this overscan, or use a smaller viewport. Exact fullscreen bounds are not a portable X11 stacking guarantee.

## SVG controls

The shipped example combines all policies in one overlay. The `mouseRegions()` helper in `index.html` samples `SVGGeometryElement.isPointInFill/isPointInStroke` in viewport CSS coordinates and merges scanlines into native rectangles. Holes and transformed geometry are supported at one CSS pixel resolution. DOM controls use bounding rectangles. The host still consumes rectangles, not selectors or SVG paths. Apps must republish after geometry/layout changes. Clipping, masks, non-scaling strokes and animated geometry need application-specific regions. The example uses `requestPointerLock()` for mouse gestures, hides the pointer and restores its starting position on release. Touch/pen input uses `setPointerCapture`. The macOS and Linux hosts authorize Pointer Lock only for standalone overlay windows. A rejected lock aborts mouse adjustment with an error.

## Main shortcut routing

```js
await reaper.window.setShortcutRouting(true);
const enabled = (await reaper.window.getState()).shortcutRouting;
// Lua: reaper.ReaWeb_SetShortcutRouting(id, true)
```

This opt-in policy keeps native keyboard focus in the WebView. Outside editable fields, trusted letter, digit, punctuation, navigation, function and numeric-keypad key presses route to the user's REAPER **Main** section bindings, including Shift/Ctrl/Alt and macOS Command combinations. It does not simulate a fixed Play action or redirect keys to the MIDI editor section. Press ownership and initial modifiers remain fixed through repeat/release. Blur, disable, navigation and close release held routed keys.

Text inputs, textareas, contenteditable, editable Shadow DOM, focused iframes and IME composition retain page input. Click an editable field to type, then click a non-editable control to resume Main shortcuts. Unbound routed keys are consumed outside editable fields. OS-reserved combinations, browser/platform shortcuts handled before DOM delivery (including DevTools shortcuts), media keys and arbitrary international keyboard layouts are outside the routing guarantee.

The default is `false`, including after reload. `setShortcutRouting(false)` restores the existing keyboard policy and returns `false` on success. While enabled, it takes precedence over `setKeyboardCapture` and the older Space-only `setDawKeyboard` for eligible presses. Neither setting is overwritten. Hosted JSFX Web UIs reject this new setter with `WINDOW_HOSTED`. Lua returns the enabled state, with errors in `ReaWeb_GetLastError()`. Inspect `capabilities.runtime.window.shortcutRouting` for availability. This routing does not require mouse passthrough to release native keyboard focus.
