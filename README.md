# ReaWebAPI

**English** | [简体中文](README.zh-CN.md)

A native REAPER 6.60+ extension for running local HTML/CSS/JavaScript in a dockable WebView. Lua opens the page. The injected `reaper` object calls native APIs through Promises.

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

Each native file is available separately. Platform ZIPs include the demo and SDK. `ReaWebAPI-ReaPack-v0.1.2.zip` contains only seven native files in `extension/` and `ReaWebAPI.ext`, ready to copy into the ReaScripts repository. The `.ext` is also a separate release asset.

For the demo, merge a platform ZIP into the REAPER resource directory and load `Scripts/ReaWebAPI/Example/Example.lua` in the Action List. The demo follows track selection automatically. Its pan slider demonstrates coalesced writes, **Center pan (Undo)** demonstrates an Undo batch, and **Runtime diagnostics** shows host state. **Dock / Undock** moves the live page into or out of REAPER's Docker.

## API

```lua
local file = debug.getinfo(1, "S").source:sub(2)
local directory = file:match("^(.*[/\\])")
local id = reaper.ReaWebOpen(directory .. "index.html")
if id == 0 then reaper.ShowConsoleMsg(reaper.ReaWeb_GetLastError() .. "\n") end
```

Lua exposes `ReaWeb_Close`, `ReaWeb_IsOpen`, `ReaWeb_IsReady`, `ReaWeb_Focus`, `ReaWeb_DevTools`, `ReaWeb_SetDocked`, `ReaWeb_IsDocked` and `ReaWeb_GetDiagnostics`. Each takes a window ID. `SetDocked` also takes a boolean and returns the resulting docked state. `GetDiagnostics` returns JSON. Relative paths resolve from `<resource>/Scripts/`. Asynchronous loading errors appear in the REAPER console.

```javascript
await reaper.ready;
const track = await reaper.GetSelectedTrack(0, 0);
if (track) console.log(await reaper.GetTrackName(track));
const unsubscribe = await reaper.ReaWeb_On('selectionchange', state => console.log(state.count));
```

No bridge import is needed. The current API covers track counts, selection, names and `D_VOL`, `D_PAN`, `B_MUTE`, `I_SOLO`, plus window controls and `GetAppVersion()`. See [TypeScript declarations](runtime/reaper.d.ts) or call `ReaWeb_GetCapabilities()` for the full list. JavaScript window controls target the current page, without an ID. `ReaWebOpen(path)` opens another page relative to the current HTML directory.

Only the current project (`0` or `null`) is supported. Track handles belong to one window and project. Reacquire them after deleting tracks or switching projects. Errors reject with `code`, `message` and optional `details`. Reloading discards the old document queue. Project switches and file reloads reject stale requests. Requests expire after 25 seconds if execution has not started. Started calls cannot be cancelled. Check the resulting state after a timeout and do not automatically retry writes.

Common host interfaces:

| Capability | JavaScript |
| --- | --- |
| Readiness, diagnostics | `ready`, `ReaWeb_GetDiagnostics()` |
| Window control | `ReaWeb_Focus()`, `ReaWeb_SetTitle(title)`, `ReaWeb_GetWindowState()` |
| Keyboard policy | `ReaWeb_SetKeyboardCapture(boolean)`, enabled by default. Disable to follow REAPER's shortcut rules |
| Events | `ReaWeb_On(name, callback)`, returns an idempotent async disposer |
| Batches, Undo | `ReaWeb_Batch(calls, { undoLabel })`, up to 32 calls |
| Continuous controls | `ReaWeb_SetTrackValueLatest(track, key, value)`, superseded waiting values resolve with `superseded: true` |

Events include `projectchange`, `selectionchange` and `windowstatechange`, with an initial snapshot and coalesced updates. Project and selection checks run about every 100 ms. Use them to refresh UI, not to record every edit. Batches validate arguments before execution and close Undo and UI refresh scopes synchronously. A partial failure returns `BATCH_FAILED` with completed results, without rolling back earlier writes. Coalescing is opt-in and leaves ordinary API calls unchanged. It does not create an Undo group for a drag gesture.

## Runtime

Windows uses WebView2, macOS uses WKWebView, and Linux runs WebKitGTK in a separate shared process. All windows share one browser profile. Windows/Linux data lives under `<resource>/ReaWebAPI/WebViewData/`. On macOS that directory stores the profile ID, while WebKit manages the actual storage location.

Floating windows are owned by REAPER. Docking preserves the page and its JavaScript state. Position, size, maximization and docking are saved under `<resource>/ReaWebAPI/WindowState/`, keyed by page path and concurrent instance slot. Restoration clamps the window to an available screen. Windows/Linux support the demo's **Developer Tools** button. On macOS, enable Safari's developer features and inspect the REAPER page through its **Develop** menu. macOS builds are ad-hoc signed, without notarization.

REAPER APIs run on the main thread. Bridge dispatch rotates between windows with a 2 ms soft budget per tick. A single native call or docking operation cannot be preempted. Bridge JSON parsing, serialization and state file writes run on a worker. Messages and queues have fixed limits.

Load trusted local pages only. ReaWebAPI exposes an explicit subset of REAPER APIs, not the entire native API. Browser profiles are shared, so prefix storage keys with your tool's name.

## Build and release

Requires CMake 3.24+, C++17, Python 3 and platform development tools. Linux also needs `pkg-config`, `libwebkit2gtk-4.1-dev` and `libx11-dev`. Dependency revisions are pinned in `CMakeLists.txt`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix dist/install
```

On Windows, configure with MSVC x64 (`-A x64`). On macOS, pass `-DCMAKE_OSX_ARCHITECTURES=arm64` or `x86_64`.

Pushing to the default branch builds all five targets and publishes the version in `CMakeLists.txt` to **Releases**. Matching `v*` tags and manual runs on the default branch also publish. Pull requests only build. Published versions are left intact. Bump the version for a new release.

The submission whitelist keeps local tests, dependency caches and build output out of Git. CI builds from committed source alone. See [third-party notices](THIRD_PARTY.md).
