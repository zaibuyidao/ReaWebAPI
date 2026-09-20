# v0.1.8 Runtime API

[Runtime API 完整清单 / Complete inventory](runtime-api-inventory.md) · [TypeScript](../runtime/runtime-api.d.ts)

**English** | [简体中文](runtime-api.zh-CN.md)

JavaScript Runtime fixes thirteen namespace boundaries: `reaper.window`, `reaper.theme`, `reaper.dialog`, `reaper.events`, `reaper.lifecycle`, `reaper.debug`, `reaper.fs`, `reaper.audio`, `reaper.clipboard`, `reaper.dragDrop`, `reaper.app`, `reaper.system`, `reaper.transaction`. All 730 REAPER methods retain their names, Promise results, typed handles and Lua argument/result order. Distribute `reaper.d.ts`, `reaper-api.generated.d.ts` and `runtime-api.d.ts` together. `capabilities.runtime.contract` is 2 (host SDK), while `webRuntime.contract` remains 1 (Web resources/storage).

Use `await reaper.lifecycle.ready` for the handshake. Batches and managed Undo belong to `reaper.transaction`; coalesced mixer controls belong to `reaper.audio`. `reaper.system.getCapabilities()` reports capabilities and `reaper.debug.setBufferSize(bytes)` configures fixed native output buffers. The [host services reference](host-api.md) details their existing argument, error and cleanup contracts.

All thirteen namespaces provide implemented members: 63 methods and one Promise property. `capabilities.runtime.reservedNamespaces` is empty. See the [API inventory](runtime-api-inventory.md).

`transaction` provides batching and Undo groups. The name does not promise atomic rollback or isolation: completed writes remain applied and other edits may interleave. See the [complete API inventory](runtime-api-inventory.md).

## Files and clipboard

`reaper.fs` provides `readText`, `writeText`, `readBinary` and `writeBinary` with explicit encoding, plus overloaded `readFile` / `writeFile`, `stat`, `readDirectory`, `makeDirectory`. Text is UTF-8 and binary data is `Uint8Array`. Writes accept `{overwrite:true}` to replace an existing file; the default refuses overwrite. The same 16 MiB limits and worker-based file operations apply. Clipboard text is read/written with `reaper.clipboard.readText()` / `reaper.clipboard.writeText(text)`. External URLs use `reaper.system.openExternal(url)`.

## Window and lifecycle

`reaper.window` provides `open(path)`, `openDev(url)`, `getSize`, `setSize(width,height)`, `getPosition`, `setPosition(x,y)`, `show`, `hide`, `getState`, `setTitle`, `focus`, `setDocked`, `isDocked`, `setKeyboardCapture`, `close` and `reload`. Use `setDocked(true)` to dock and `setDocked(false)` to undock; both return the actual docked state, so successful undocking returns false. New-window methods resolve to a window ID; controls act on the calling window without an ID. Bounds are outer window dimensions and screen coordinates in native desktop units, not CSS pixels. Size limits are 100–16384; coordinates are -1000000–1000000, clamped to the desktop work area. Docker geometry belongs to REAPER: setters reject with `WINDOW_DOCKED`. Hiding affects the App container, not the entire REAPER window.

```js
await reaper.window.setSize(900, 700);
const stop = await reaper.lifecycle.on('before-close', async () => {
  await reaper.fs.writeFile('settings.json', JSON.stringify(settings), {overwrite:true});
});
await reaper.lifecycle.on('cleanup', () => worker.terminate());
```

Lifecycle events are `before-close`, `before-reload` and `cleanup`; all run before document destruction. Await registration. Callbacks may return Promises and receive `{reason,timeoutMs}`. The relevant before-* and cleanup callbacks start together and are awaited concurrently; place ordered save/release work in one callback. The combined deadline is 2000 ms, with native fallback cleanup regardless of callback failures. Close cannot be vetoed. Apps without listeners close directly. Do not recursively close/reload from cleanup callbacks.

Native close buttons, bridge close requests and same-document reloads participate. The bridge remains usable during cleanup. Reload invalidates old requests, handles, subscriptions and audio jobs. Forced unload/process exit/crash cannot guarantee asynchronous persistence; pagehide only attempts synchronous cleanup with reason `unload` and timeout 0. Persist important state during normal operation.

## Events

`await reaper.events.on(name, callback)` returns an async disposer. The event catalogue includes:

| Name | Meaning |
| --- | --- |
| `track-added`, `track-deleted` | Changes to current-project track GUID sets, after establishing a baseline |
| `track-selected` | Track-selection snapshot, equivalent to `selectionchange` |
| `item-changed`, `take-changed` | Project-wide cache invalidation, not a per-object edit journal; may include unrelated edits |
| `playback-state-changed` | Play-state changes separate from continuously moving position |
| `tempo-changed` | Current master tempo, not individual tempo-map edits |
| `marker-changed`, `fx-changed` | Native change notifications requiring a refresh; not every plugin-internal mutation |
| `project-loaded` | Host-observed project load generation; tab switches use `projectchange` |
| `project-saved` | Observed current project file update and clean state, excluding Undo-only serialization and backup files; not exactly-once delivery |
| `theme-changed` | Theme colors and CSS variables |

Host callbacks only update thread-safe counters. REAPER queries and JS dispatch remain on the main thread. Fallback scans are subscription-driven and incremental. Native track-selection notifications and project change counters are checked on each main-thread tick. Selection polling remains as a 100 ms fallback for silent edits. Other state scans run around 100 ms and theme checks around 500 ms. Delivery is asynchronous and bounded by the host scheduler, scan budget and WebView response time. Track/load/save transitions have no historical replay. Same-name notifications may coalesce, so GUID arrays are not a lossless delta journal. Other events can supply initial snapshots. Close/reload clears subscriptions.

## Dialogs, theme and debug

`reaper.dialog.openFile`, `saveFile` and `selectFolder` wrap the standard REAPER `GetUserFileName` native dialogs. Options: `title`, `initialPath`, `filters:[{name,extensions:['wav','flac']}]`. Cancellation returns null. Saving selects a path without writing it. REAPER interprets initial paths; prefer absolute ones. A missing host function rejects with `API_UNAVAILABLE`.

`reaper.theme.getColors()` returns `{available,colors,cssVariables}` for background, text, highlight, panel and border. `reaper.theme.apply(element?)` applies and follows `--reaper-*` variables; its disposer restores prior inline values. `reaper.events.on('theme-changed', callback)` subscribes without applying styles. Missing theme APIs return documented fallback colors and `available:false`; REAPER's theme is never modified.

`reaper.debug.log/warn/error` write bounded previews to REAPER's console and a 200-entry per-window log. `inspect` handles circular values. `getLogs` and `getDiagnostics` expose JS/native errors, request IDs, document generations, cleanup state and audio job counts. Uncaught JS errors and unhandled rejections are recorded without replacing console. Nothing is uploaded.

`reaper.debug.openDevTools()` requests showing the inspector on Windows/Linux. On macOS it shows a Safari guide and rejects with `INSPECTOR_MENU`. See [DevTools](devtools.md) for shortcuts, layouts and the `getDiagnostics()` field `devtools`.

## Audio

```js
const info = await reaper.audio.getFileInfo('/path/to/audio.wav');
const waveform = await reaper.audio.getWaveform(info.path, {points:1024});
const track = await reaper.GetSelectedTrack(0, 0);
const meter = track ? await reaper.audio.getTrackMeter(track) : null;
```

File information contains path, sampleRate, channels, bitDepth, duration (seconds) and format. Unknown/not-applicable bit depth is null. Decoders come from REAPER. Audio sources must have 1–32 channels; MIDI is not decoded as audio.

Waveform options are points (1–8192, default 1024), start and duration in seconds. The default covers the whole file. The result adds start, rangeDuration, points, requestedPoints, peaksPerSecond and data: one `{min:number[],max:number[]}` per channel. Values are linear amplitude. Draw point i at `start + i / peaksPerSecond`, using actual returned points. Top-level duration remains the complete file length. Empty/out-of-range requests fail.

Jobs own independent PCM sources without adding tracks/items. REAPER calls stay on the main thread; peak building advances one native step per tick, with JSON encoding on the existing worker. There are at most 8 queued jobs and a 120-second waveform deadline. Close/reload releases sources and finishes active peak builders. Individual host decoder calls cannot be preempted and may still block on slow storage. REAPER may create its own `.reapeaks` data; ReaWebAPI adds no waveform cache.

Track meters return per-channel peak and peakDb; silence is null in dB. They are snapshots, not RMS/LUFS or an audio stream. Deleted/foreign-document handles are rejected. Errors include `AUDIO_UNSUPPORTED`, `AUDIO_PEAKS_UNAVAILABLE`, `AUDIO_INVALID_DATA`, `AUDIO_TIMEOUT`, `FILE_NOT_FOUND` and `QUEUE_LIMIT`. Unavailable peaks never become fabricated silence. Advanced analysis, DSP, real-time streaming and batch analysis are deferred.

## App identity, data and system

All five App getters return Promises. `getId()` reuses the stable browser-storage identity of the canonical local entry directory (the trusted URL in development mode). Windows in that App share the ID and data directory; reopening preserves them. Moving the directory or changing the development URL creates a different identity. This is a local storage identity, not a publisher-assigned global UUID; a manifest does not override it.

`getRootPath()` returns the absolute local entry/resource directory. Development pages use the local launch base (REAPER's Scripts directory for Lua launchers). `getName()` reads optional `name` from that directory's `app.json`, falling back to its directory name. `getVersion()` reads optional `version`, otherwise null; it never returns the extension version as the App version. Metadata is read when the App is created, shared by its open windows, and refreshed after all its windows close and it is reopened. Invalid metadata rejects opening with `APP_MANIFEST_INVALID`; the file must be a JSON object no larger than 64 KiB. Name must be nonblank and at most 256 characters; version follows the manifest schema's `N.N.N[-suffix]` format. Complete manifest/entry validation remains a separate tool; these getters do not install or launch manifests.

`getDataPath()` returns `<REAPER resource>/ReaWebAPI/Apps/<appId>/Data`, created for the App independently of web resources and browser caches. Paths are native absolute paths, without a promised trailing separator. Use the existing file API, for example `await reaper.fs.writeText((await reaper.app.getDataPath()) + '/settings.json', '{}', {overwrite:true})`. This directory is intended for configuration, cache and state; trusted Apps can still access other local paths. Directory creation failures reject with `APP_DATA_UNAVAILABLE`; subsequent filesystem failures retain normal file errors.

`system.getPlatform()` returns `windows`, `macos` or `linux`. `getArchitecture()` describes the running extension process, including emulation: `x64`, `arm64`, `x86`, `arm`, or `unknown`. `revealInFileManager(path)` resolves an existing local file/directory relative to the entry directory, or accepts an absolute path. Windows Explorer/macOS Finder select it; Linux uses FileManager1 ShowItems, falling back to opening its parent when unavailable. Resolves true when the OS accepts the request, not when its UI has finished loading. Errors include `INVALID_PATH`, `FILE_NOT_FOUND`, `REVEAL_FAILED`.

## Native drag/drop and callback removal

The namespace is **`reaper.dragDrop`** (camelCase), without a `dragdrop` alias. `startFiles(paths)` starts a native **copy** drag of 1–256 existing regular files; relative paths use the entry directory, duplicates are removed. `startText(text)` starts a native copy drag of nonempty text, up to 16 MiB UTF-8 without NUL. Both return `Promise<boolean>`: true for an accepted copy, false for cancellation/rejection. Acceptance does not guarantee that the target imported or decoded a file. ReaWebAPI never deletes source files or automatically inserts REAPER items.

Start from a pointer/mouse-down handler while the left button remains held; resolve file selections before that gesture. Do not invoke it from a click (the button is already released). An absent gesture rejects with `DRAG_GESTURE_REQUIRED`; only one native drag runs at a time (`DRAG_BUSY`). Closing destroys the source; reloading invalidates its old Promise. Backend errors reject with `DRAG_FAILED` or `HOST_UNAVAILABLE`.

```js
button.addEventListener('pointerdown', event => {
  if (event.button !== 0) return;
  event.preventDefault();
  reaper.dragDrop.startFiles([selectedAudioPath]).catch(error => reaper.debug.error(error));
});
const dropped = payload => console.log(payload.files, payload.text, payload.x, payload.y);
const dispose = await reaper.events.on('native-drop', dropped);
await reaper.events.off('native-drop', dropped);
await dispose(); // Still safe after off.
```

`reaper.events.on('native-drop', callback)` returns an asynchronous disposer. The callback receives `{files:string[], text:string, x:number, y:number}` with absolute native file paths (incoming directories allowed), optional text as an empty-or-populated string, and viewport CSS coordinates. It reads no file contents. Native OS/REAPER sources must offer standard file or text data; proprietary REAPER object formats are not decoded. The receive side captures drops while subscribed; with no subscription, normal WebView DOM drop handling applies. Native drops have no initial snapshot/history replay and are not coalesced. Close/reload clears listeners. Windows requires WebView2 native additional-file-object support; unsupported runtimes reject subscription with `HOST_UNAVAILABLE`.

`events.off(name, callback)` returns `Promise<void>` and removes all registrations of that exact callback for that name, including pending registration; other callbacks/names are unchanged. Missing registrations are a no-op. Callback execution already in progress is not aborted. The disposer returned by `events.on()` cancels an individual subscription. `debug.log` writes entries at the `info` level.

## Manifest validation and example

`SDK/app-manifest.schema.json` defines name/version/entry with optional schemaVersion and author. Run `python tools/validate_app.py runtime/runtime-demo/app.json` in source, or `python tools/validate_app.py runtime-demo/app.json` inside the SDK. Validation is read-only and checks field types, existence and resolved entry containment.

Manifest files do not automatically launch/install an App; existing HTML launchers remain the entry point. No app manager, updater, installer or permission enforcement is included. Unsupported permissions fields are rejected. Apps remain trusted; the resource-server root is not a native API sandbox.

Run Open.lua beside [Runtime Studio](../runtime/runtime-demo/index.html) for an unbundled example covering windows, events, theme, dialogs, audio waveform, manual meters, diagnostics and cleanup-state persistence.
