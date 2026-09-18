# ReaWebAPI

**English** | [简体中文](README.zh-CN.md)

A native REAPER extension for running local HTML/CSS/JavaScript in a dockable WebView. Lua opens the page. The injected `reaper` object calls native APIs through Promises.

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

Each native file is available separately. Platform ZIPs include the demo and SDK. `ReaWebAPI-ReaPack-v0.1.1.zip` contains only seven native files in `extension/` and `ReaWebAPI.ext`, ready to copy into the ReaScripts repository. The `.ext` is also a separate release asset.

For the demo, merge a platform ZIP into the REAPER resource directory and load `Scripts/ReaWebAPI/Example/Example.lua` in the Action List. Select a track and click **Get selected track name**. **Dock / Undock** moves the live page into or out of REAPER's Docker.

## API

```lua
local file = debug.getinfo(1, "S").source:sub(2)
local directory = file:match("^(.*[/\\])")
local id = reaper.ReaWebOpen(directory .. "index.html")
if id == 0 then reaper.ShowConsoleMsg(reaper.ReaWeb_GetLastError() .. "\n") end
```

Lua also exposes `ReaWeb_Close(id)`, `ReaWeb_IsOpen(id)`, `ReaWeb_DevTools(id)`, `ReaWeb_SetDocked(id, boolean)` and `ReaWeb_IsDocked(id)`. `SetDocked` returns the resulting docked state. Relative Lua paths resolve from `<resource>/Scripts/`. A window ID means initialization was accepted. Asynchronous failures appear in the REAPER console.

```javascript
const track = await reaper.GetSelectedTrack(0, 0);
if (track) console.log(await reaper.GetTrackName(track));
await reaper.ReaWeb_SetDocked(true);
```

No bridge import is needed. The current API covers track counts, selection, names and `D_VOL`, `D_PAN`, `B_MUTE`, `I_SOLO`, plus window controls and `GetAppVersion()`. See [TypeScript declarations](runtime/reaper.d.ts) or call `ReaWeb_GetCapabilities()` for the full list. JavaScript window controls target the current page, without an ID. `ReaWebOpen(path)` opens another page relative to the current HTML directory.

Only the current project (`0` or `null`) is supported. Track handles belong to one window and project. Reacquire them after deleting tracks or switching projects. Errors reject with `code` and `message`. A timeout does not cancel a queued native call, so do not automatically retry writes.

## Runtime

Windows uses WebView2, macOS uses WKWebView, and Linux runs WebKitGTK in a separate shared process. All windows share one browser profile. Windows/Linux data lives under `<resource>/ReaWebAPI/WebViewData/`. On macOS that directory stores the profile ID, while WebKit manages the actual storage location.

Floating windows are owned by REAPER. Docking preserves the page and its JavaScript state. Windows/Linux support the demo's **Developer Tools** button. On macOS, enable Safari's developer features and inspect the REAPER page through its **Develop** menu. macOS builds are ad-hoc signed, without notarization.

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
