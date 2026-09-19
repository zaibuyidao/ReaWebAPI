# Web Runtime v1 and frontend resources

**English** | [简体中文](frontend.zh-CN.md) · [Host API](host-api.md)

ReaWebAPI introduced the **Web Runtime v1** contract in v0.1.6 and preserves it in **v0.1.7**. The Web contract version is independent of the extension version. The 730 standard REAPER 7.80 mirror bindings and their Promise-based calling convention remain unchanged. No custom Lua RPC or third-party REAPER API registration is included.

## Plain Web Apps

Use ordinary HTML, CSS and JavaScript directories. No bundler or npm is required:

```text
MyApp/
  Open.lua
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

The [unbundled check App](../runtime/web-runtime/README.md) demonstrates modules, local fetch, browser storage, Canvas, file objects, DOM drop events and Workers. Run its `Open.lua` in REAPER. The extension injects the bridge into the top-level page; do not import `reaper.js`.

## Guaranteed baseline and optional capabilities

The v1 support contract is the required column below, on supported and maintained platform WebViews. Engine-specific new JavaScript syntax and every API that happens to be exposed are not part of this baseline.

| Capability | v1 contract | Notes |
| --- | --- | --- |
| HTML5, CSS, ES6+, DOM/events, Promise, async/await, JSON | Required | Native browser implementation; advanced/new features still need feature detection |
| ES modules, static/dynamic imports | Required | Relative URLs, valid JavaScript MIME, no build step required; bare package imports need an import map or build |
| Local `fetch` | Required | Read resources within the App root; JSON/text/ArrayBuffer; missing files return 404 |
| Timers, requestAnimationFrame | Required | Normal browser throttling applies in hidden/minimized windows; not an audio clock |
| Canvas 2D | Required | Native browser context |
| localStorage | Required | Persistent native profile, isolated by App directory; native quota/errors apply |
| File, Blob, FileReader, ArrayBuffer | Required | Browser file objects; arbitrary disk access uses host file APIs |
| Standard DOM Drag & Drop | Required | Handle `dragover`/`drop` and `DataTransfer`; does not imply a REAPER-to-OS native drag export API |
| External HTTP/HTTPS fetch, WebSocket | Optional | Remote CORS, certificates, CSP, network and browser restrictions apply; no native proxy or CORS bypass |
| IndexedDB, classic Worker, module Worker | Optional | Use feature detection and handle runtime errors/quotas |
| WebGL | Optional | Depends on browser, GPU, driver and session |
| Bridge calls inside Workers/iframes | Not supplied | Send Worker messages to the top-level page and call `reaper` there |

No special support is added for Service Worker, PWA, push, geolocation, camera, microphone, WebRTC, payment, Bluetooth or USB. An engine may expose some constructors; that is not a ReaWebAPI support promise. Node.js/npm/Electron APIs are not part of the runtime.

## Resource origin and storage

`reaper.window.open(path)` still accepts a local HTML path. Its canonical parent directory is the **App root**. A read-only native listener serves that directory on `http://127.0.0.1:<port>/`; the WebView performs normal HTTP requests. Relative assets, Unicode/space/#/% file names, query strings, MIME types, HEAD and byte ranges are supported. Encode `#` and `%` in resource URLs (for example `file%23%25.js`). The server has no directory listing, write or bridge endpoint. Paths escaping the root, including resolved symlinks/junctions, are rejected.

Each root gets one profile and one saved origin. Windows in the same directory share browser storage. Different directories use separate profiles, including separate cookie jars: distinct ports alone would not isolate cookies. Each bridge document still owns its own handles and subscriptions. Do not persist native handles; store settings or GUIDs.

The profile registry is `<REAPER resource>/ReaWebAPI/Apps/<appId>/`. `origin.json` records the root and port. Windows/Linux browser data is in its `WebViewData/` directory. macOS stores a WKWebsiteDataStore UUID there; WebKit owns the actual database location. Reopening or restarting preserves the origin and profile. Moving/renaming the App directory creates a different identity. Upgrades in the same directory retain it.

If a saved port is occupied, opening fails with `APP_ORIGIN_BUSY` instead of silently changing origin and losing access to storage. Close the conflicting process and reopen. Malformed/mismatched origin metadata fails with `APP_ORIGIN_INVALID`. Do not delete that record as a routine fix: a new port gives a new origin. Normal browser quotas and clearing data still apply.

Older `file://` storage in `ReaWebAPI/WebViewData/` is left untouched and is **not automatically migrated**. Export existing settings from an older version before changing versions if needed. Keep browser data and origin metadata when preserving an App's state.

Only the loopback interface is bound. Requests with a foreign Host/Origin or cross-origin Fetch Metadata are rejected; there is no permissive CORS header. Serving stops when the last App window closes. The listener uses two bounded worker threads per active App; REAPER calls continue through the existing main-thread bridge. The root is a resource boundary, **not a sandbox for trusted native APIs**: pages retain the exposed filesystem and REAPER privileges. Do not put secrets in a directory you serve or open untrusted Apps.

Both `reaper.system.getCapabilities()` and `reaper.debug.getDiagnostics()` expose `webRuntime`:
`{ contract: 1, mode: 'app-http' | 'dev-http', appId, origin, storageIsolation: 'app-profile', localResources }`.
`localResources` is true for the built-in local resource origin.

## TypeScript and development servers

The [modern starter](../runtime/modern/README.md) supplies Vite/TypeScript as optional **build-time** tooling. Run `npm ci`, `npm run dev`, then `OpenDev.lua`. The default URL is `http://localhost:5173/`. `reaper.window.openDev` accepts explicit-port HTTP URLs on 127.0.0.1, localhost or [::1]. Start the server yourself. ReaWebAPI does not embed Vite, Node or npm.

Development URLs have separate profiles keyed by the exact URL. Use a stable entry URL and port. Hash navigation is allowed; navigation to a different document is blocked. Avoid entry redirects. HMR uses the browser's native WebSocket and has been exercised with Vite.

Run `npm run build` and `Open.lua` for production; ship `Open.lua` and `dist/`. The current template emits an IIFE and inline Blob Worker as one packaging choice; ordinary ES module Apps work as well. Both modes use native `fetch('./data.json')`. Native host file paths resolve from the local HTML directory (`dist/` here); a Lua-opened development page uses REAPER's Scripts directory. A browser fetch URL and a native file path are different namespaces.

A page CSP must allow its scripts/styles, `connect-src 'self'` for local fetch and the appropriate `worker-src`. The modern template's Blob Worker needs `worker-src blob:`. Development additionally needs its Vite HTTP/WebSocket URLs. Host clipboard and external-link helpers provide the cross-platform native operations.

## Platform validation and differences

| Platform | Backend/profile | v0.1.6 local validation |
| --- | --- | --- |
| Windows x64 | WebView2, per-App user-data folder | Required check App, storage reopen/host-process restart/isolation, IndexedDB, both Workers, HTTP CORS fetch and WebSocket round-trip, WebGL; native plugin in mock REAPER |
| Linux x86_64 | WebKitGTK 4.1, X11/XWayland; separate helper per App | Required checks, browser process restart/isolation, HTTP CORS/WebSocket and Workers passed on WebKitGTK 2.52.6/WSLg with the renderer compatibility setting below |
| macOS 14+ | WKWebView, per-App persistent data-store UUID | Implemented with public APIs; requires macOS build and real-host acceptance, not verified on this Windows machine |

WebView2 follows the installed runtime; WKWebView follows macOS updates; WebKitGTK follows distribution packages. Linux needs its matching helper beside the extension. Optional capability behavior can differ with engine versions and graphics environments. HTTPS transport is delegated to the WebView; the automated network fixture verifies HTTP CORS and WebSocket, not arbitrary external TLS endpoints. DOM drop checks use synthetic events; physical OS file drops and real REAPER drag gestures remain manual acceptance cases.

On macOS the displayed origin uses `http://localhost:<port>/` while the listener is still bound only to 127.0.0.1. macOS 14+ ATS restricts IP-literal HTTP; an unqualified local hostname avoids changing the REAPER application's Info.plist. Prefer `localhost` in macOS development URLs too. See [Apple's local networking rules](https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking).

In the tested WSLg environment, WebKitGTK's DMA-BUF renderer stalled animation frames despite a visible document. Running the helper with `WEBKIT_DISABLE_DMABUF_RENDERER=1` passed the full browser suite, including native requestAnimationFrame and WebGL. This is an environment setting, not a default forced by the extension or a JS polyfill. Validate the default renderer on your target Linux desktop; when affected, set the variable before launching REAPER. Native GTK viewport allocation is synchronized with the foreign X11 parent's client size.

Implementation: `src/web_resources.*` handles local resources; `src/runtime.*` owns App origin/profile lifetimes; `src/platform_win.cpp`, `src/platform_mac.mm` and `src/linux_webkit.cpp` select native profiles and load the entry; `runtime/reaper.d.ts` describes the runtime metadata. Tests cover the native HTTP boundary, all 730 mirror ABI mappings and actual browser behavior.

Native behavior references: [WebView2 local content](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/working-with-local-content), [WebKitGTK persistent cookies](https://webkitgtk.org/reference/webkit2gtk/2.42.5/method.CookieManager.set_persistent_storage.html). Native cookie persistence is explicitly enabled on Linux.
