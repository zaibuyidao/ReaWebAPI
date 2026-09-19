# ReaWebAPI

**English** | [简体中文](README.zh-CN.md)

A native REAPER 6.68+ extension for running local HTML/CSS/JavaScript in a dockable WebView. Lua opens the page. The injected `reaper` object calls native APIs through Promises.

## Install

Download from [Releases](https://github.com/zaibuyidao/ReaWebAPI/releases). Choose the architecture of your **REAPER process**.

| Platform | Extension | Runtime |
| --- | --- | --- |
| Windows x64 | `reaper_reawebapi-x64.dll` | Windows 10/11, WebView2 Evergreen, VC++ x64 runtime |
| macOS ARM64 | `reaper_reawebapi-arm64.dylib` | macOS 14+ |
| macOS Intel | `reaper_reawebapi-x86_64.dylib` | macOS 14+ |
| Linux x64 | `reaper_reawebapi-x86_64.so` | Ubuntu 24.04 or compatible, WebKitGTK 4.1, X11/XWayland |
| Linux ARM64 | `reaper_reawebapi-aarch64.so` | Same Linux requirements |

Quit REAPER, place the extension in its resource directory's `UserPlugins/`, then restart. Linux also needs the matching `reawebapi-webview-<arch>` helper beside the `.so`.

Each native file is available separately. Platform ZIPs include the demo and SDK. `ReaWebAPI-ReaPack-v0.1.8.zip` contains only seven native files in `extension/` and `ReaWebAPI.ext`, ready to copy into the ReaScripts repository. The `.ext` is also a separate release asset.

Merge a platform ZIP into the REAPER resource directory and load `Scripts/ReaWebAPI/Example/Example.lua` in the Action List. The demo includes project, track and FX queries, color/pan Undo operations and docking. **Run read-only checks** exercises ten paths covering handles, tuples, GUIDs, rectangles, MIDI bytes and audio buffers. Select a track containing a MIDI item to cover every path. Empty projects show explicit skips.

## Developer resources

[SDK and starter](runtime/README.md) · [Developer guide](docs/development.md) · [730 API reference](docs/api-reference.md) · [Host API](docs/host-api.md)

The standalone `ReaWebAPI-SDK-v<version>.zip` contains editor declarations, a runnable starter, API definitions and documentation. Platform ZIPs include these files too. The extension-only ReaPack ZIP remains unchanged.

## API

```lua
local file = debug.getinfo(1, "S").source:sub(2)
local directory = file:match("^(.*[/\\])")
local id = reaper.ReaWeb_Open(directory .. "index.html")
if id == 0 then reaper.ShowConsoleMsg(reaper.ReaWeb_GetLastError() .. "\n") end
```

Lua exposes `ReaWeb_Close`, `ReaWeb_IsOpen`, `ReaWeb_IsReady`, `ReaWeb_Focus`, `ReaWeb_DevTools`, `ReaWeb_SetDocked`, `ReaWeb_IsDocked` and `ReaWeb_GetDiagnostics`. Each takes a window ID. `SetDocked` also takes a boolean and returns the resulting docked state. `GetDiagnostics` returns JSON. Relative paths resolve from `<resource>/Scripts/`. Asynchronous loading errors appear in the REAPER console.

```javascript
await reaper.lifecycle.ready;
const track = await reaper.GetSelectedTrack(0, 0);
if (track) {
  const [ok, name] = await reaper.GetTrackName(track);
  if (ok) console.log(name);
}
const [total, markers, regions] = await reaper.CountProjectMarkers(0);
const [beat, bar] = await reaper.TimeMap2_timeToBeats(0, await reaper.GetCursorPosition());
```

No bridge import is needed. **All 730 standard REAPER 7.80 APIs have native bindings.** Arguments and results follow Lua signature order. One result resolves to a scalar, multiple results to an array, and void to `undefined`. See the [SDK declarations](runtime/reaper-api.generated.d.ts). Older REAPER versions report `API_UNAVAILABLE` for missing functions. Check `reaper.system.getCapabilities().api.available` and `.unavailable`. Use REAPER 7.80 or newer for all 730 functions.

Use `0` or `null` for the current project, or a project handle returned by `EnumProjects` for another open project. Tracks, items, takes, envelopes and resources use typed handles, never raw addresses. Reacquire deleted objects and handles from reloaded documents. MIDI bytes use `Uint8Array`. Audio buffers accept `Float64Array` or `number[]` and are updated before the Promise resolves. GUIDs use strings. RECT arguments follow the four coordinates in the Lua signature.

**Migration:** `GetTrackName` now returns `[ok, name]`. Supply project and index arguments explicitly, as documented by REAPER. Ordinary track value calls accept the full native parameter set.

JavaScript window controls target the current page without an ID. `reaper.window.open(path)` resolves relative to the current HTML directory. Errors reject with `code`, `message` and optional `details`. Project switches reject stale queued calls. Requests expire after 25 seconds if execution has not started. Execution acknowledgement stops the queue timer, so native dialogs and renders can finish normally. Started calls cannot be cancelled. Do not automatically retry timed-out writes.

Host APIs:

| Capability | JavaScript |
| --- | --- |
| Readiness, diagnostics | `reaper.lifecycle.ready`, `reaper.debug.getDiagnostics()` |
| Window control | `reaper.window.focus()`, `reaper.window.setTitle(title)`, `reaper.window.getState()` |
| Keyboard policy | `reaper.window.setKeyboardCapture(boolean)`, enabled by default. Disable to follow REAPER's shortcut rules |
| Events | `reaper.events.on(name, callback)`, returns an idempotent async disposer |
| Batches, Undo | `reaper.transaction.batch(calls, { undoLabel })`, up to 128 calls |
| Fixed output buffers | `reaper.debug.setBufferSize(bytes)`, 64 KiB default, 16 MiB maximum |
| Continuous controls | `reaper.audio.setTrackValueLatest(track, key, value)`, superseded waiting values resolve with `superseded: true` |

Events now include track/item/take selection, transport, FX, project and window state. Batches expose 173 reviewed standard APIs with result references, current-project validation and paired Undo/refresh cleanup. Managed Undo gestures survive browser awaits and are closed by the host on reload/close or timeout. Files, clipboard and external links have common host APIs; TypeScript/Vite development uses an explicit loopback entry. See the [host reference](docs/host-api.md), [frontend contract](docs/frontend.md) and [v0.1.8 changes](docs/release-notes.md).

v0.1.7 fixes 13 typed Runtime namespace boundaries: `reaper.window`, `reaper.theme`, `reaper.dialog`, `reaper.events`, `reaper.lifecycle`, `reaper.debug`, `reaper.fs`, `reaper.audio`, `reaper.clipboard`, `reaper.dragDrop`, `reaper.app`, `reaper.system`, `reaper.transaction`. All thirteen provide implemented APIs, including App identity/data paths, native file/text drag/drop, system information and events.off; debug.info is removed in favor of log. See the [Runtime API](docs/runtime-api.md) and run `SDK/runtime-demo/Open.lua` for Runtime Studio.

## Runtime

Windows uses WebView2, macOS uses WKWebView, and Linux runs WebKitGTK in a separate process per App. Local Apps use stable loopback HTTP origins for native ES modules and local fetch. Windows in the same App directory share a profile; different directories are isolated. Profile metadata lives under `<resource>/ReaWebAPI/Apps/<appId>/`. See the [Web Runtime v1 contract](docs/frontend.md) for persistence, migration and platform requirements.

Floating windows are owned by REAPER. Docking preserves the page and its JavaScript state. Position, size, maximization and docking are saved under `<resource>/ReaWebAPI/WindowState/`, keyed by page path and concurrent instance slot. Restoration clamps the window to an available screen. Windows/Linux support the demo's **Developer Tools** button. On macOS, enable Safari's developer features and inspect the REAPER page through its **Develop** menu. macOS builds are ad-hoc signed, without notarization.

REAPER APIs run on the main thread. Bridge dispatch rotates between windows with a 2 ms soft budget per tick. A single native call or docking operation cannot be preempted. Bridge JSON parsing, serialization and state file writes run on a worker. Messages and queues have fixed limits.

Pages have the full capabilities of the exposed APIs, including project writes and file operations. Load trusted local pages. Browser storage is isolated by App directory; native filesystem access remains privileged.

## API definition maintenance

The independent [API Sync Tool](api/README.md) maintains definitions and differences. `tools/native_bindings.py` lowers native arguments and generates C++ during the build. CMake checks the complete schema, bindings and official SDK types offline. Unmapped arguments and signature drift block the build. Runtime JSON installation is unnecessary.

```sh
python -m tools.api_sync check --require-complete
python -m tools.api_sync report
```

API Sync `update` never accepts binding changes automatically. See the maintenance guide for the review workflow.

## Build and release

Requires CMake 3.24+, C++17, Python 3.10+ and platform development tools. Linux also needs `pkg-config`, `libwebkit2gtk-4.1-dev` and `libx11-dev`. Dependency revisions are pinned in `CMakeLists.txt`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix dist/install
```

On Windows, configure with MSVC x64 (`-A x64`). On macOS, pass `-DCMAKE_OSX_ARCHITECTURES=arm64` or `x86_64`.

Pushing to the default branch builds all five targets and publishes the version in `CMakeLists.txt` to **Releases**. Matching `v*` tags and manual runs on the default branch also publish. Pull requests only build. Published versions are left intact. Bump the version for a new release.

Regression tests are committed and run on every CI platform before packaging/release. Dependency caches and build output stay out of Git. Run `ctest --test-dir build -C Release --output-on-failure` after configuring with `-DBUILD_TESTING=ON`. See [third-party notices](THIRD_PARTY.md).

JavaScript uses `reaper.window.open(path)` and `reaper.lifecycle.ready`, with no flat Runtime aliases. Lua bootstrap remains `reaper.ReaWeb_Open(path)`; these native Lua functions are separate from the browser SDK. The 730 standard REAPER mirror names are unchanged.

[Runtime API 完整清单 / Complete inventory](docs/runtime-api-inventory.md) — 13 implemented namespaces, 67 methods, 1 Promise property.

v0.1.8 organizes native sources into `core/`, `runtime/`, `platform/`, `web/` and `plugin/`, with focused Runtime implementation files and a shared session library for the extension and tests. See the [source layout](docs/source-layout.md) and [release notes](docs/release-notes.md).
