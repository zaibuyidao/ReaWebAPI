# Web Runtime v1 and frontend resources

**English** | [简体中文](frontend.zh-CN.md) · [Host API](host-api.md)

The **Web Runtime v1** contract defines browser capabilities, resource loading and storage. Its version is independent of the extension version. JavaScript calls the 730 standard REAPER 7.80 APIs through Promises.

## WebView Color Profile

**REAPER Preferences > Plug-ins > ReaWebAPI** opens **ReaWebAPI settings**. **Use sRGB for WebView rendering on Windows** is unchecked by default, preserving WebView and system color management. On Windows, checking it requests a fixed sRGB display profile to reduce UI color differences from REAPER and ReaImGui. **Restore defaults** clears the checkbox. Apply/OK saves the change, and Cancel discards unapplied changes, including Restore defaults.

| Platform | Backend | sRGB override |
| --- | --- | --- |
| Windows | WebView2 | Passes `--force-color-profile=srgb` when creating the shared browser environment. Applies to all ReaWebAPI App and development WebViews. |
| macOS | WKWebView | No equivalent public API. The control is disabled and native color management remains active. |
| Linux | WebKitGTK 4.1 | No equivalent public API. The control is disabled and native color management remains active. |

On Windows, Apply/OK saves `[ReaWebAPI] WebViewColorProfile=Default|sRGB` in `ReaWebAPI.ini` in the REAPER resource directory. Restart REAPER after changing it. Reloading a page or reopening an App does not apply a pending change. The existing shared browser data directory is retained.

The Windows override uses a [WebView2 browser flag](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/webview-features-flags), whose behavior and availability may change with the runtime. Host environment or registry overrides can take precedence. It does not change the monitor's ICC profile, HDR configuration or other plug-ins. It cannot guarantee identical physical colors on every display. [WKWebView](https://developer.apple.com/documentation/webkit/wkwebview) and [WebKitGTK settings](https://webkitgtk.org/reference/webkit2gtk/stable/class.Settings.html) provide no matching per-WebView display-profile setting. This release does not implement an override for those backends.

## Mouse pointer while typing

On Windows, ReaWebAPI respects **Hide pointer while typing**. Typing in a WebView hides the pointer, including when it rests over REAPER's Arrange View. Moving it over a REAPER native window restores visibility and the native cursor shape without changing keyboard focus. A stationary pointer remains hidden until movement. No App-side handler or system-setting change is required.

The Windows backend forwards a non-client movement notification to the focused WebView2 window when native pointer movement would otherwise leave its typing cursor hidden. macOS WKWebView and Linux WebKitGTK retain their native pointer handling. Restart REAPER after updating the extension.

## Plain Web Apps

On Windows and macOS, initial display waits for page loading and a native page snapshot. This avoids exposing an empty native/WebView surface while initial HTML and styles load. Gradients, background images and the final CSS canvas color are preserved. Asynchronous App data loaded after the document's `load` event remains the App's responsibility. Initialization failures or a missing initial frame after 15 seconds close the pending window and report an error. Linux retains the v0.3.8.10 startup path and shows the container after native registration without waiting for a page snapshot.

Docked and floating WebViews synchronize their native resize background with an opaque CSS canvas background on `html`, or on `body` when propagated to the canvas. Root/body attribute changes, stylesheet loads and style-node changes, viewport resizing and system color-scheme changes refresh this color. Transparent or translucent canvas backgrounds retain the browser's white fallback. Background images and gradients remain browser-rendered. No App-side resize handler is required.

Windows, macOS and Linux share this background synchronization path for non-transparent windows. Computed `rgb()`/`rgba()` colors with integer sRGB channels and alpha 0 or 1 are parsed directly. Other colors use Canvas, preserving color conversion and 8-bit alpha rounding. Explicit `{transparent:true}` windows bypass this path.

Use ordinary HTML, CSS and JavaScript directories. No bundler or npm is required:

```text
MyApp/
  reawebapi-my-app.lua
  index.html
  app.js
  ui.js
  style.css
  data/config.json
  images/
```

```html
<link rel="stylesheet" href="./style.css">
<script type="module" src="./app.js"></script>
```

```js
import { render } from './ui.js';
const config = await (await fetch('./data/config.json')).json();
localStorage.setItem('theme', 'dark');
await reaper.lifecycle.ready;
const track = await reaper.GetTrack(0, 0);
render(config, track);
```

The [unbundled check App](../examples/runtime-features/web-runtime/README.md) demonstrates modules, local fetch, browser storage, Canvas, file objects, DOM drop events and Workers. Run its `reawebapi-web-runtime.lua` in REAPER. The extension injects the bridge into the top-level page; do not import `reaper.js`.

## Guaranteed baseline and optional capabilities

The v1 support contract is the required column below, on supported and maintained platform WebViews. Engine-specific new JavaScript syntax and every API that happens to be exposed are not part of this baseline.

| Capability | v1 contract | Notes |
| --- | --- | --- |
| HTML5, CSS, ES6+, DOM/events, Promise, async/await, JSON | Required | Native browser implementation; advanced/new features still need feature detection |
| ES modules, static/dynamic imports | Required | Relative URLs, valid JavaScript MIME, no build step required; bare package imports need an import map or build |
| Local `fetch` | Required | Read resources within the App root; JSON/text/ArrayBuffer; missing files return 404 |
| Timers, requestAnimationFrame | Required | Normal browser throttling applies in hidden/minimized windows; not an audio clock |
| Canvas 2D | Required | Native browser context |
| localStorage | Required | Persistent storage isolated by origin in the shared profile; native quota/errors apply |
| File, Blob, FileReader, ArrayBuffer | Required | Browser file objects; arbitrary disk access uses host file APIs |
| Standard DOM Drag & Drop | Required | Handle `dragover`/`drop` and `DataTransfer`; does not imply a REAPER-to-OS native drag export API |
| External HTTP/HTTPS fetch, WebSocket | Optional | Remote CORS, certificates, CSP, network and browser restrictions apply; no native proxy or CORS bypass |
| IndexedDB, classic Worker, module Worker | Optional | Use feature detection and handle runtime errors/quotas |
| WebGL | Optional | Depends on browser, GPU, driver and session |
| Bridge calls inside Workers/iframes | Not supplied | Send Worker messages to the top-level page and call `reaper` there |

No special support is added for Service Worker, PWA, push, geolocation, camera, microphone, WebRTC, payment, Bluetooth or USB. An engine may expose some constructors; that is not a ReaWebAPI support promise. Node.js/npm/Electron APIs are not part of the runtime.

## App-level cross-origin isolation

Set `"webRuntime": { "crossOriginIsolation": "required" }` in the App root's `app.json`. The default is `"off"`. Unknown `webRuntime` fields, values and invalid types fail with `APP_MANIFEST_INVALID`. The policy is read when the first App window opens and retained through reloads and additional windows. Close **all** App windows before applying a configuration change.

Required resources receive `Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp` and `Permissions-Policy: cross-origin-isolated=(self)`. Windows retains `reaweb://<appId>`. On macOS/Linux, required Apps use an authenticated `http://127.0.0.1:<port>` origin, persistently bound to the App ID in `Apps/<appId>/isolation-origin.json`. A saved port conflict fails with `APP_ORIGIN_IN_USE`, without changing origin. Ordinary Apps retain their custom-scheme origins. External scripts, images, fonts and media need compatible CORS/CORP responses. App CSP remains authoritative. Use `worker-src 'self' blob:` for URL/Blob Workers and `script-src 'self' 'wasm-unsafe-eval'` for WASM where supported.

App IDs, private files and window state remain unchanged. Browser storage is origin-specific. Enabling required mode on macOS/Linux starts separate localStorage/IndexedDB storage and retains the old data without automatic migration. Export existing browser state before switching modes if it is needed. Returning to off mode restores the previous origin. Keep `app.json` and the persistent origin record with the App's data. Linux requires a WebKitGTK engine with isolated-process SharedArrayBuffer support. See [the private runtime build](linux-webkit-runtime.md). Required HTTP documents use the existing stream transport for project video instead of custom-scheme frame delivery.

The document-start guard checks the browser's actual isolation state. Failure stops loading and reports `CROSS_ORIGIN_ISOLATION_FAILED`, or `CROSS_ORIGIN_ISOLATION_UNSUPPORTED` when the browser does not expose isolation. The host closes the failed window and records the error in REAPER Console and native Runtime Diagnostics, including failures before the API handshake. Ordinary Apps are unaffected.

```js
await reaper.lifecycle.ready;
const { webRuntime } = await reaper.system.getCapabilities();
console.log(webRuntime.crossOriginIsolation, webRuntime.browser);
```

`crossOriginIsolation` separates `requested`, backend `supported`, document `active`, `reason`, initialization `stage`, `failureStage` and `documentGeneration`. A `null` support/state value means unverified, not success. `browser` reports actual secure-context, isolation, SAB, shared WASM memory, URL Worker, Blob Worker and WebGL2 results. Worker probes exchange messages and shared memory. `browser.workers` contains their isolation/memory results. `browser.errors` distinguishes creation/send, loading/execution, message, shared-memory, resource and timeout failures. A timed-out or unavailable probe is `null`, a verified failure is `false`.

`getCapabilities()` and `debug.getDiagnostics()` await and reuse one browser probe per document. Required Apps also start this probe after the handshake. Document readiness, Worker and resource probes have three-second timeouts and release Workers and Blob URLs. Native diagnostics can show `null` while probes are pending. Development HTTP pages report their actual document isolation but cannot use the reserved local Worker probe resource. WebGL2 is independent of WASM threads and is required by some applications, including the PSP core.

Use standard `Worker`, `SharedArrayBuffer`, `Atomics` and `WebAssembly.Memory`. No threading namespace or Worker bridge is added. Apps terminate unused Workers. The browser releases dedicated Workers when their document closes or reloads. Do not use blocking `Atomics.wait()` on the page thread.

The [threaded WASM example](../examples/runtime-features/isolation/README.md) covers four URL Workers, four Blob Workers, atomic WASM execution, wait/notify, errors and persistence. See the [Windows](windows-isolation-validation.md) and [WebKit acceptance records](webkit-isolation-validation.md) for tested environments and remaining limits. Headers alone are not proof of engine support.

See the [Windows acceptance record](windows-isolation-validation.md) for emulator behavior, the PSP core correction and stability coverage.

## Resource origin and storage

For browser File/Blob playback, native file selection and opt-in download exports, see [browser files and media](browser-media.md).

`reaper.window.open(path)` accepts a local HTML path. Its canonical parent directory is the **App root**. Production pages normally use `reaweb://<appId>/<entry>`. Required macOS/Linux Apps use the persistent loopback origin described above. Both paths serve the same read-only resources. Relative URLs, ES modules, local fetch, Unicode/space/#/% names, MIME, HEAD and single byte ranges are supported. Encode `#` and `%` in resource URLs, for example `file%23%25.js`. Directory listing, writes, traversal and symlinks/junctions outside the root are rejected.

Define a stable, globally unique `id` in the root's `app.json`:

```json
{ "id": "timefold", "name": "TimeFold", "version": "1.0.5" }
```

This produces `reaweb://timefold/index.html`, and `reaper.app.getId()` returns `timefold`. IDs contain only lowercase ASCII `a-z`, `0-9` and `-`. Display names and directory names do not define identity. Assign a new ID when copying a template to create a different App.

When `id` is absent, ordinary WebUIs use a persistent random `app-<32 lowercase hex digits>` ID stored in `<App root>/.reawebapi/identity.json` as `{"schema":1,"appId":"app-…"}`. The directory moves with the App and is excluded from Web resource responses, including resolved aliases. Marker creation and binding checks are serialized. An unreadable or malformed marker fails with `APP_IDENTITY_INVALID`. Failure to create or update it fails with `APP_IDENTITY_UNAVAILABLE`, without a transient-ID fallback. `app.json` is never modified.

Moving the directory with its marker preserves identity. A complete copy receives a new ID when the copied ID is already bound to another valid installation. If the old binding is invalid, the existing recovery rules apply instead. Uncertain filesystem status, corrupt bindings and active windows using an invalid old binding remain errors. Deleting the marker initializes a new identity on the next open.

Lua source and `instanceKey` retain their path-resolution and window-reuse roles but do not generate App IDs. JSFX WebUI continues to derive its fallback ID from `fx_ident`, including when `app.json` exists without `id`. An explicit valid `app.json.id` takes precedence for both types. `app.json.entry` identifies an HTML entry, not an App ID.

**Upgrade:** ordinary WebUIs without an explicit ID or identity marker receive a new random ID. There is no lookup, fallback or migration from launcher-derived IDs. Their previous localStorage, IndexedDB, Data and WindowState are retained but are not automatically associated with the new identity. Explicit-ID Apps are unaffected.

Two valid installations cannot bind the same ID, even after closing their windows. The second root fails with `APP_ID_CONFLICT`. A binding can recover automatically when the old root is missing or its recorded HTML entry is missing or no longer a regular file, provided no active or initializing window uses it. Root comparisons follow platform path rules. Uncertain filesystem status fails safely, and conflict diagnostics include both roots and the identity record path. Changing an ID creates a different origin and data directory.

Legacy schema 2 records without `entry` remain supported. A missing old root can be recovered when unused. If the old root still exists, its `app.json.entry` can identify the original entry. Without reliable entry metadata, the runtime retains `APP_ID_CONFLICT` rather than inferring the original entry from other HTML files or the new request. Successful normal opens backfill the actual entry. Recovery preserves App ID, origin, private data, window-state files and the shared browser profile.

All Apps share the existing browser profile. localStorage and IndexedDB are isolated by the App's actual origin and persist across restarts. Cookie behavior remains native to each engine. Custom schemes may not support cookies, so use localStorage or IndexedDB for persistent App state. Each bridge document retains its own handles and subscriptions.

```text
<REAPER resource>/ReaWebAPI/
  WebViewData/
  Apps/
    <appId>/
      origin.json
      Data/
      WindowState/
```

Windows shares one WebView2 Environment. Linux shares one WebKitGTK context and helper. macOS retains the shared WKWebsiteDataStore UUID in `WebViewData/`, with database locations managed by WebKit. `origin.json` schema 2 records the App ID, root and virtual origin, with no port. New or updated bindings also store `entry`, a normalized relative HTML/HTM path confined to the root, including resolved symlinks and junctions. Existing schema 1 records at the same identity path can be upgraded. Old hashed App directories and browser data are retained, but HTTP-origin storage is not automatically transferred to the new virtual origin. Malformed identity metadata fails with `APP_ORIGIN_INVALID`.

Back up the shared profile and relevant `Apps/<appId>/` directories together. Normal browser quotas and user data clearing still apply. Development pages retain their explicit HTTP origin and create no production origin record. External Client listeners, configuration and authentication are independent.

Resource handlers reject foreign App authorities and origins without permissive CORS. The App root limits resource loading, not the trusted native API's filesystem privileges. REAPER calls continue through the existing main-thread bridge.

Capabilities and diagnostics expose:
`{ contract: 1, mode: 'app-virtual' | 'app-loopback' | 'dev-http', appId, origin, storageIsolation: 'origin', localResources }`.
Production Apps report their actual origin and `localResources: true` in both local-resource modes.

## Navigation

The entry document stays in the WebView on Windows, macOS and Linux. Blocked navigation to external `http://`, `https://` and `mailto:` URLs uses the same validation and system handler as `reaper.system.openExternal(url)`. This covers ordinary links and JavaScript redirects such as `location.href` and `location.assign()`.

Local `file:` URLs and other paths or queries on the App/dev entry origin remain blocked, with a developer-console warning. Use `reaper.window.open(path)` to open another local HTML document. Hash-only navigation remains allowed. Query changes still use the existing entry-document identity check. `window.open()` and links requesting a new window remain unsupported.

```html
<a href="https://www.extremraym.com">Open website</a>
```

```js
window.location.assign('https://www.extremraym.com'); // Opens the system browser.
await reaper.window.open('other.html');
```

## TypeScript and development servers

The [modern starter](../examples/templates/modern/README.md) supplies Vite/TypeScript as optional **build-time** tooling. Run `npm ci`, `npm run dev`, then `reawebapi-modern-dev.lua`. The default URL is `http://localhost:5173/`. `reaper.window.openDev` accepts explicit-port HTTP URLs on 127.0.0.1, localhost or [::1]. Start the server yourself. ReaWebAPI does not embed Vite, Node or npm.

Development App identities are keyed by the exact URL. Browser storage is isolated by origin in the shared profile, so different entry URLs on the same origin share storage. Use a stable entry URL and port. Hash navigation is allowed; navigation to a different document is blocked. Avoid entry redirects. HMR uses the browser's native WebSocket and has been exercised with Vite.

Run `npm run build` and `reawebapi-modern.lua` for production; ship `reawebapi-modern.lua` and `dist/`. The current template emits an IIFE and inline Blob Worker as one packaging choice; ordinary ES module Apps work as well. Both modes use native `fetch('./data.json')`. Native host file paths resolve from the local HTML directory (`dist/` here); a Lua-opened development page uses REAPER's Scripts directory. A browser fetch URL and a native file path are different namespaces.

A page CSP must allow its scripts/styles, `connect-src 'self'` for local fetch and the appropriate `worker-src`. The modern template's Blob Worker needs `worker-src blob:`. Development additionally needs its Vite HTTP/WebSocket URLs. Host clipboard and external-link helpers provide the cross-platform native operations.

Chromium DevTools also uses `connect-src 'self'` to read `/.well-known/appspecific/com.chrome.devtools.json`. The App resource handler returns empty settings when the file is absent. An App's own file takes precedence when configuring an automatic workspace.

## Platform backends

| Platform | Backend | Storage isolation |
| --- | --- | --- |
| Windows x64 | WebView2 | Shared profile, distinct App origins |
| Linux x64 / ARM64 | WebKitGTK 4.1, X11/XWayland | Shared context and helper, distinct App origins |
| macOS ARM64 / Intel | WKWebView | Shared persistent data store, distinct App origins |

WebView2 follows the installed runtime; WKWebView follows macOS updates; WebKitGTK follows distribution packages. Linux needs its matching helper beside the extension. Optional capability behavior can differ with engine versions and graphics environments. HTTPS transport is delegated to the WebView; the automated network fixture verifies HTTP CORS and WebSocket, not arbitrary external TLS endpoints. DOM drop checks use synthetic events; physical OS file drops and real REAPER drag gestures remain manual acceptance cases.

Ordinary production origins use `reaweb://<appId>`. Required macOS/Linux Apps use the persistent loopback origin described above. Prefer `localhost` for macOS HTTP development URLs. See [Apple's local networking rules](https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking).

In the tested WSLg environment, WebKitGTK's DMA-BUF renderer stalled animation frames despite a visible document. Running the helper with `WEBKIT_DISABLE_DMABUF_RENDERER=1` passed the full browser suite, including native requestAnimationFrame and WebGL. This is an environment setting, not a default forced by the extension or a JS polyfill. Validate the default renderer on your target Linux desktop; when affected, set the variable before launching REAPER. Native GTK viewport allocation is synchronized with the foreign X11 parent's client size.

Implementation: `src/web/web_resources.*` handles local resources; `src/runtime/runtime.*` owns App origins and the shared profile lifetime; `src/platform/windows/platform_win.cpp`, `src/platform/macos/platform_mac.mm` and `src/platform/linux/linux_webkit.cpp` select native profiles and load the entry; `runtime/reaper.d.ts` describes the runtime metadata. Tests cover the native resource boundary, all 730 mirror ABI mappings and actual browser behavior.

Native behavior references: [WebView2 local content](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/working-with-local-content), [WebKitGTK persistent cookies](https://webkitgtk.org/reference/webkit2gtk/2.42.5/method.CookieManager.set_persistent_storage.html). Native cookie persistence is explicitly enabled on Linux.

Hidden startup loads the same resources and Runtime without showing the native window. Readiness remains a document handshake, independent of rendering or App data. See [hidden startup](host-api.md#hidden-startup) and the [preload example](../examples/runtime-features/hidden-start/README.md).

## Native drag regions

For a window using `{frameless:true}` or `window.setFrameless(true)`, add `data-reaweb-drag` to a rectangular title region and `data-reaweb-no-drag` to custom interactive areas. Buttons, links, inputs, selects, textareas, editable/focusable elements and scrollable descendants are excluded automatically. Explicitly exclude custom controls, overlays and Shadow DOM hosts. Keep a visible close control calling `reaper.window.close()`.

```html
<header data-reaweb-drag>
  <span>My App</span>
  <button id="close">Close</button>
  <div data-reaweb-no-drag>Custom controls</div>
</header>
<script>
  document.querySelector('#close').onclick = () => reaper.window.close();
</script>
```

Windows uses WebView2 native non-client regions. macOS and Linux cache clipped DOM rectangles before the press and consume the native mouse event directly. They do not start native movement from an asynchronous Runtime call. Keep title regions stable during interaction. Layout snapshots have a 32 ms sampling interval, with at most 512 rectangles in each drag/exclusion list. Oversized snapshots disable dragging. Arbitrary shapes and animated hit regions are outside this contract. Drag regions are inactive inside Docker containers. Right-click outside a drag region retains the existing page context-menu behavior. See the [Demo](../examples/windows/frameless/README.md).

## Transparent backgrounds

Open with `{transparent:true}` and explicitly set `html, body { background:transparent; }`. The host does not rewrite App CSS. Use translucent panels, rounded corners and CSS shadows within transparent page margins. Keep a visible close control in frameless tools. Visual transparency retains rectangular mouse hit testing. See the [transparent Demo](../examples/windows/transparent/README.md) and [native requirements](host-api.md#transparent-windows).
