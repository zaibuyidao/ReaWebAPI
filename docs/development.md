# Developer guide

**English** | [简体中文](development.zh-CN.md) · [Documentation](README.md)

## Start a tool

Install the extension for your REAPER architecture, then copy the SDK directory to a writable development location. Load its `starter/reawebapi-starter.lua` in the Action List. This opens `index.html` next to the Lua file. The starter reads the selected track. Right-click the page and toggle **Dock <App> in Docker**. The checked state indicates docking.

```text
SDK/
  reaper.d.ts
  runtime-api.d.ts
  starter/
    reawebapi-starter.lua
    index.html
    app.js
    style.css
    jsconfig.json
```

No web server, npm install or build step is needed for this JavaScript template. The Lua action can finish immediately after `reaper.ReaWeb_Open`: the extension owns the window. The starter passes its launcher source as `instanceKey`, so rerunning it focuses the existing page. See [window instances](host-api.md#window-instances) for named windows and explicit multiple instances. The page runs in a browser environment, not Node.js. Ordinary browsers do not provide `reaper.window.reaper`.

Use relative paths for bundled assets and a deferred classic script (`<script src="app.js" defer></script>`). If using a bundler, emit files suitable for a local HTML entry point. Test asset loading, module loading and any network requests on each target WebView. A development server URL cannot be passed to `reaper.window.open`, which accepts local `.html` or `.htm` files.

The starter's `jsconfig.json` enables JavaScript type checking without compilation. See the [TypeScript checkJs documentation](https://www.typescriptlang.org/tsconfig/checkJs.html). For TypeScript, include all three `.d.ts` files and compile your `.ts` files to browser JavaScript. The SDK declares a global object, so do not `import reaper`.

## EEL2 backend

The SDK includes a [minimal EEL2 backend](../examples/backends/eel-backend/README.md) and a [bidirectional track mixer](../examples/backends/eel-mixer/README.md). Load their `.eel` scripts in the Action List. No Lua launcher or additional dependencies are needed. Both examples use `ReaWeb_SetFrame` for state refresh, `0..4` volume gain, and acknowledged slider writes with bounded pending work and final-value protection.

Since v0.3.9.14, `ReaWeb_Open` argument four accepts EEL2 `0/1` and Lua `false/true`, with unchanged omission behavior. Native string returns use the first EEL2 output parameter, such as `ReaWeb_Receive(#message, id)`. Both direct and `extension_api` calls work. Stop `defer` after window closure and clean up with `atexit`. See [EEL2 support](eel-backend-compatibility.md) for the full interface, string limits and verification commands.

## Calling conventions

Web pages access the host through the runtime namespaces on `window.reaper`. Authenticated external WebSocket clients can discover named services, events and streams through `system.getCapabilities`.

```javascript
await reaper.lifecycle.ready;
const track = await reaper.audio.getSelectedTrackRef();
if (track) {
  const {name} = await reaper.audio.getTrackMix(track);
  console.log(name);
  await reaper.transaction.withUndo('Center pan', () =>
    reaper.audio.setTrackValueLatest(track, 'D_PAN', 0));
}
```

Read the project name and track count with `reaper.host.service('runtime').invoke('getProjectSummary')`.

## Events, UI and storage

```javascript
const stop = await reaper.events.on('selectionchange', state => {
  console.log('Selected tracks:', state.count);
});
// When this UI component is removed:
await stop();
```

Subscriptions deliver an initial snapshot and coalesced changes. Project changes and native track-selection notifications are checked on each main-thread tick, with a 100 ms selection fallback for silent edits. These are UI refresh signals, not an edit history or sample clock. Catch failures inside asynchronous callbacks. Dispose subscriptions when a component unmounts. Page closure clears the document's subscriptions.

Docking preserves page state. Apps share one browser profile. localStorage and IndexedDB are isolated by origin, while cookies follow native domain and path rules. Use the [Web Runtime contract](frontend.md) for origin persistence and storage rules. Persist GUIDs or tool settings rather than object handles, and resolve persisted references against the appropriate project on the next run. Browser storage and network behavior follow the platform WebView. ReaWebAPI does not add Node.js filesystem or shell APIs.

For standard WASM threads, opt into [App cross-origin isolation](frontend.md#app-level-cross-origin-isolation) and inspect `webRuntime.browser` before choosing a threaded build. The [example](../examples/runtime-features/isolation/README.md) includes a restrictive App CSP and actual four-Worker atomic computation.

## Errors and debugging

Bridge failures reject with `Error` plus `code` and optional `details`. Native API return values such as `false`, `0` or `null` retain the meaning documented by REAPER and do not automatically become exceptions.

```javascript
try {
  const track = await reaper.audio.getSelectedTrackRef();
  if (track) console.log(await reaper.audio.getTrackMix(track));
} catch (error) {
  console.error(error); // Includes code/details for bridge failures.
}
```

Use `await reaper.debug.getDiagnostics()` to inspect backend, stage, queue counts and the last host error. Press Ctrl+Shift+I on Windows/Linux or Option+Command+I on macOS in the WebView or Inspector to show or hide DevTools. Call `reaper.debug.openDevTools()` to request opening it. Windows/macOS page context menus also control visibility and Embedded/Floating mode. See [DevTools](devtools.md) for layouts, saved preferences and platform limits. Close and reopen the page after changing assets. Reloading creates a new document and invalidates its old handles.

An unstarted native request expires after 25 seconds, with a 30-second client watchdog. Once execution starts, the queue timer stops. A native dialog or render cannot be interrupted by JavaScript. Never retry a timed-out write automatically. Check state first. Limits and error codes are listed in the [host reference](host-api.md#errors-and-limits).

## Distribute your tool

For multi-window regression, run `python tests/multi_window_stress.py --reaper <executable> --extension <binary> --output <new-directory> --counts 8,16,32 --seconds 30`. Counts include one official Demo window. It uses an isolated profile, tests empty and synthetic 50-track/80-JSFX projects, and runs independent Lua loops with bounded traffic, frame refresh and out-of-order close/reopen. `cases.tsv`, `windows.tsv` and diagnostic snapshots record latency and per-window progress. Synthetic DSP load does not reproduce third-party sampler workloads.

`latency.tsv` separates steady-phase message RTT, Lua polling intervals and frame-content age at receipt of the page's echo. Frame age includes return transport. `--no-frames` disables frame subscriptions in the traffic windows for comparison, leaving the official Demo unchanged. Use the same REAPER executable and workload for before/after comparisons.

Linux geometry invalidation is covered by `linux_geometry` in CTest. On an X11 display, build and run `linux_geometry_live` for native move/resize, hide/show and window recreation. In WSL without DRI3, only the WebKit helper defaults to `GDK_RENDERING=image`. An existing value is preserved. This selects GTK software backing surfaces without changing WebKit's acceleration settings. See [GTK rendering modes](https://docs.gtk.org/gtk3/running#environment-variables).

Ship the Lua launcher, HTML, built JavaScript, CSS and required assets, preserving relative paths. Rename the starter's action description and `<title>` in `index.html` for your tool. See [window titles](runtime-api.md#window-titles) for dynamic names and explicit overrides. The `.d.ts` files and editor configuration are only needed by developers. Users need the native ReaWebAPI extension, not Python or a compiler.

State your minimum REAPER and ReaWebAPI versions. Feature-detect required methods and explain missing dependencies in the UI. Test empty projects, missing selection, Unicode paths, deleted objects, project switches, docking, repeated open/close and each supported OS. Use the larger bundled Demo for bridge diagnostics. A passing simulated ABI test is not proof of all native side effects on real projects.

`ReaWebAPI-ReaPack-v<version>.zip` contains `ReaWebAPI.ext` alongside `extension/` with seven native files and the complete `web/` Demo folder. The SDK ZIP contains development materials.

The descriptor lists every Demo file and installs them under `web/` in the package's Scripts directory. `ReaWebAPI_Demo.lua` is registered in the Action List. Other resources retain their relative paths. The launcher uses `@noindex` to stay part of the extension package.

## Modern frontend and host I/O

See the [frontend contract](frontend.md) for Vite/TypeScript, loopback development, native local-resource fetch and Workers. The [host reference](host-api.md#files-and-desktop-services) covers common file, clipboard and external-link APIs.
