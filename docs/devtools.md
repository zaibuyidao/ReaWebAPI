# DevTools

**English** | [简体中文](devtools.zh-CN.md)

Press **Ctrl+Shift+I** on Windows/Linux or **Option+Command+I** on macOS in the WebView or its managed DevTools window to show or hide DevTools. Holding the keys does not repeat the toggle. Hiding returns focus to the page. `reaper.debug.openDevTools()` and `ReaWeb_DevTools(id)` request opening or showing DevTools. Repeated API calls do not hide it.

| Platform | Presentation | Mode switching |
| --- | --- | --- |
| Linux / WebKitGTK | Right-hand panel by default, with a draggable divider | **Float DevTools** / **Embed DevTools** in the page or Docker tab context menu, or the panel toolbar |
| Windows / WebView2 | Right-hand panel by default, with a draggable divider | **Float DevTools** / **Embed DevTools** in the page context menu |
| macOS / WKWebView | Right-hand panel by default, with a draggable divider | **Float DevTools** / **Embed DevTools** in the page context menu |

Page, title-bar and Docker tab menus share docking, lifecycle-aware reload, DevTools, App folder and close actions, with the current docking state checked. Browser defaults remain hidden. Labels are English on all platforms. The same inspector controller supplies visibility and mode state for both menus.

## Linux

The panel defaults to 40% of the available width. Drag the divider to adjust it between 20% and 80%. Resizing the main window preserves the ratio. **Float DevTools** / **Dock right** moves the same inspector view between containers, retaining the Console and Inspector session without reloading the page.

Ctrl+Shift+I also works inside the inspector. The shortcut, **Hide DevTools**, and the floating window's close button hide the panel while retaining the session. The inspector's own close control can end the session and reset inspector state on reopening.

Floating DevTools is non-modal and associated with the current REAPER parent through X11. Stacking depends on the window manager. X11/XWayland is required.

## Windows

**Embedded** fills the right-hand panel with the WebView2 inspector content, hiding its title bar, window controls and frame without reserving space. There is no draggable caption. The panel defaults to 40% of the available width, adjustable between 20% and 80% with a 1px divider in the system window-frame color. Resizing preserves the ratio. **Floating** restores the inspector's native title bar, controls and frame without an additional host window. Mode switches reuse the same inspector window, retaining Console and Inspector state without reloading the page.

Right-click the WebView page to access DevTools alongside **Dock <App> in Docker** (checked when docked). The menu shows **Open DevTools** or **Hide DevTools** according to visibility, and **Float DevTools** when embedded or **Embed DevTools** when floating. Mode switching is disabled while the inspector is hidden or still opening. The inspector fills its container without a host toolbar, and the Demo has no separate DevTools button.

The menu, shortcut and APIs share one state manager. Ctrl+Shift+I and **Hide DevTools** hide the inspector and retain its session. The floating window's native close button ends the inspector session. Floating DevTools follows the current REAPER root owner and is raised without taking focus when another REAPER window becomes active. It is not globally always-on-top. Focusing DevTools does not suspend or reload the page. On Windows, embedded DevTools supports switching between docked WebViews, including when REAPER and WebView2 use different DPI contexts.

WebView2 exposes no public embedded-inspector controller. ReaWebAPI uses [`OpenDevToolsWindow`](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2#opendevtoolswindow) to create the inspector, identifies its native window, and clips Chromium's custom caption and frame to the measured renderer origin and client edges in Embedded mode. Per-Monitor V1 and V2 hosts use a compatible DPI container without changing REAPER or Chromium's process DPI mode. Unavailable content bounds, unsupported DPI combinations or hosting failures retain the native floating window and disable **Embed DevTools**. `devtools.fallbackReason` explains the fallback. If identification or inspector keyboard handling fails, `devtools.lastError` reports the limitation. An unidentified window must be closed with its own close button. This integration depends on the WebView2 native window implementation.

## macOS

**Embedded** places the native Inspector content view beside the page without a window title bar, window buttons, wrapper frame or reserved decoration space. **Floating** returns the same view to WebKit's own Inspector window. No additional ReaWebAPI window or toolbar is created.

The page context menu shows **Open DevTools** / **Hide DevTools** and **Float DevTools** / **Embed DevTools** according to the current state. **Option+Command+I** toggles visibility from the page or Inspector. Mode switching is disabled while the inspector is hidden. The shortcut, menu, `reaper.debug.openDevTools()` and `ReaWeb_DevTools(id)` use the same controller. Hiding and switching modes retain the live Inspector view and connection, including Console entries and the selected tab. Native close controls end the session.

The panel defaults to 40% of the available width. Dragging the Inspector divider adjusts the saved ratio between 20% and 80%. Resizing preserves the ratio, including in compact windows. Host dimensions do not disable **Embed DevTools** or force Floating.

Public `WKWebView.inspectable` remains enabled. Programmatic Inspector control and presentation use runtime-checked WebKit private interfaces because the public API has no equivalent controller. AppKit lays out the content view, and Inspector dock controls share the existing mode and width preferences. This integration depends on the system WebKit version. If embedding controls are unavailable, the native floating window remains usable and `embeddedSupported` is `false`. If native control is unavailable, menu actions are disabled, `nativeToggleSupported` is `false`, and `openDevTools()` rejects with `DEVTOOLS_UNAVAILABLE`. Safari's Develop menu remains available for manual inspection. `fallbackReason` and `lastError` describe capability or opening failures.

## Saved preferences and diagnostics

`ReaWebAPI/Apps/<appId>/WindowState/*.json` saves `devtools.mode` (`embedded` or `floating`) and `devtools.widthRatio` per page and window slot. Preferences survive reopening the tool and restarting REAPER. DevTools starts closed. Debugging sessions are not persisted. All platforms restore the saved mode. Windows/macOS hosting fallbacks do not overwrite the saved preference.

`(await reaper.debug.getDiagnostics()).devtools` reports the effective mode, width ratio, embedding support and backend-specific status. On macOS, `visible` reports the local WebKit Inspector state. On Windows, it refers to the native window identified by ReaWebAPI.

Production resources retain `reaweb://<appId>/...` URLs in Console, Sources and Network. The native Inspector session, shortcuts and Embedded/Floating controls are unchanged.

Persistent logs are configured separately in REAPER Preferences. See [Runtime Diagnostics](diagnostics.md).

## Startup timing

On macOS and Linux, pre-paint editor probes contribute to the shared observer's inspection and preparation counters. The Windows-only hook and cloaking counters remain zero on these platforms.

`controllerPrewarmed` identifies a window created from a prepared blank container. `idleWebViews` reports the shared pool's `target`, `ready`, `pending`, `created`, `taken` and `failed` counts. Counts cover the current platform lifetime, not App sessions. Consuming a container is independent of page loading and presentation. Linux receives these counters asynchronously from its helper.

`(await reaper.debug.getDiagnostics()).startup` retains one bounded native timing record per window. `kind` is `create`, `navigation` or `cachedReopen`. `windowId` and `documentGeneration` identify the document, with `sessionId` and `activation` for JSFX. Milestones are milliseconds from the App open request, observed JSFX discovery, subsequent navigation or cached reactivation. Discovery timing starts after the existing scan detects an editor, excluding the preceding polling interval.

Windows, macOS and Linux share these JSFX native timing pairs. Each mark records its first observation within the current startup record. Do not pair marks across navigation or cached activation.

| Operation | Start | End |
| --- | --- | --- |
| Capability handling | `connectMs` | `capabilitiesReadyMs` |
| Parameter snapshot | `parametersStartMs` | `parametersReadyMs` |
| Editor-size lookup | `editorSizeStartMs` | `editorSizeReadyMs` |

On Windows, macOS and Linux, cached JSFX reactivation also records `bindingReadyMs`, `associationReadyMs`, `hostPreparedMs`, `sizeRestoredMs`, `cacheMatchedMs`, `reparentReadyMs`, `hostAttachedMs` and `stateNotifiedMs`. These delimit binding validation, association lookup, native preparation, size restoration, cache matching, reparenting, attachment and visibility-state dispatch. `stateNotifiedMs` records dispatch, not browser receipt.

`jsfxObserver` reports a fixed set of cumulative `{ms, calls}` counters for the discovery object's lifetime, shared by all JSFX windows. `notification`, `hook` and `scan` measure host notifications, Windows pre-paint hooks and periodic discovery. Nested counters cover `inspect`, `identifyControls`, `resolveOwnership`, `hitTest`, `match`, `prepareEditor`, `createHost`, `conceal`, `restoreSize`, `cloak` and `hideControls`. `hook`, `prepareFloating` and `cloak` are Windows-only counters for pre-paint processing, floating-window preparation and DWM cloaking. They remain zero on macOS and Linux. Concealment subphases describe initial editor preparation, not every later hide. Compare counter deltas over the same interval. Nested phases must not be added to their parents, and these counters are not per-window startup milestones.

Available milestones cover environment waiting, controller creation, navigation completion, Runtime handshake, first-frame preparation and native reveal. Windows `environmentStartup.initializationMs` is the complete environment attempt duration. The difference between `environmentReadyMs` and `environmentWaitStartMs` is this window's remaining wait, including native host construction. `mode` distinguishes a ready environment from an in-progress initialization. Navigation and cached reopen omit stages they do not repeat. On Linux, controller readiness and load completion are observations received from the WebKit helper. Linux has no separate opaque-page snapshot confirmation.

`revealCompleteMs` observes completion of the existing native reveal path, not physical display scanout or business-data readiness. JSFX `presentationMs` retains its existing meaning. Overlapping milestones must not be summed, and browser and native clocks must not be subtracted.

Use browser Navigation/Resource Timing for HTML, script and style loading. JSFX exposes bounded `performance.getEntriesByType('measure')` entries prefixed `reaweb.jsfx.` for capability readiness, subscriptions, editor size, parameters, connection, pending size requests and present acknowledgement. Each phase keeps only its latest entry, with session and activation in `detail`. `present` includes the existing pending-size wait, not physical display latency. Runtime Studio records its independent metadata queries as `reaweb.demo.appInfo`. App-specific first-screen data and layout can use separate User Timing measures. None of these measurements adds a display prerequisite.

The shared JSFX SDK also measures `lifecycle`, `subscribe.<event>`, `initialInvoke.<method>` and `initialSessionRefresh` under the `reaweb.jsfx.` prefix. Subscription durations overlap and must not be added. Initial invoke measures include reply waiting rather than just native processing. `initialSessionRefresh` records a successful capability refresh after an initial `STALE_SESSION`.

On all three platforms, non-transparent windows record the first `reaweb.background.<phase>` measure for `rootStyle`, `bodyStyle`, `parse` and any Canvas fallback (`context`, `clear`, `fill`, `read`). A missing phase was not executed, rather than measured at zero duration. See [background conversion](frontend.md#plain-web-apps).

Inspect native timing and browser measures in the hosted page's console:

```js
const { startup } = await reaper.debug.getDiagnostics();
console.log(startup);
console.table(performance.getEntriesByType('measure')
  .filter(({ name }) => /^(reaweb\.jsfx\.|reaweb\.background\.)/.test(name))
  .map(({ name, startTime, duration }) => ({ name, startTime, duration })));
```
