# ReaWebAPI

**English** | [简体中文](README.zh-CN.md)

ReaWebAPI is a native REAPER extension for building tools with HTML, CSS and JavaScript. It provides dockable WebView windows and asynchronous access to REAPER through the `reaper` object.

Space routing starts from ordinary Apps’ `app.json.dawKeyboard` (default `false`) or JSFX’s `JSFX_WebUI.dawKeyboard` (default `true`). Both support [`reaper.window.setDawKeyboard()`](docs/runtime-api.md#space-routing) for runtime overrides. Reload restores JSON configuration without writing it.

- [Frame channel](docs/frame-timer-verification.md) with native transport samples and replaceable Lua content, independent of Lua polling frequency on Windows, macOS and Linux. Small frames avoid worker round-trips, and pending state updates coalesce behind one encoded event batch.
- 730 standard REAPER 7.80 API bindings with TypeScript declarations.
- Mirror-aware Batch Builder for 456 reviewed APIs, including transport queries and time conversions, with deferred references, tuple destructuring and typed results.
- Lua or EEL2 backend + WebView UI through `ReaWeb_Send`, `ReaWeb_Receive`, `reaper.host.send` and `message` events, with fair multi-window delivery and bounded communication turns independent of project sampling.
- Optional [External Clients](docs/external-clients.md) over authenticated localhost WebSocket, sharing the Native API, Batch, Service, Event and binary Stream capabilities.
- [Native Events and Host Services](docs/native-services.md) with subscription-driven state monitoring and a C/C++ extension registry, independent of Lua loops.
- [JSFX Web UI](docs/jsfx-webui.md) hosts associated JSFX in track, master, take, input, monitoring and nested container FX, with instance-bound `params`, host parameter metadata, loaded-instance enum choices in `enum` (unavailable fields are `null`), normalized writes and grouped Undo gestures. `editor` provides persistent, shared content sizing. The optional EEL SDK adds DSP streams, structured state, commands and binary resources. The [native channel](docs/jsfx-native-channel.md) uses bounded staging programs to reduce EEL compilation overhead during initialization. Initial capability and state reads avoid an additional host scheduling cycle on Windows, macOS and Linux. SDK-managed DSP streams preserve subscriptions across session replacement and transport recovery.
- Optional [transparent floating windows](docs/host-api.md#transparent-windows) with desktop Alpha, explicit backend capability reporting and preserved Dock preferences.
- [Frameless floating windows](docs/host-api.md#frameless-windows) with live title-bar switching, native corners and drag regions on Windows, macOS and Linux X11/XWayland.
- 15 Runtime namespaces for windows, events, files, native dialogs, drag and drop, audio, Undo and application services.
- Stable `reaweb://<appId>` origins with native local resources, modules, Workers and persistent App storage, without per-App TCP listeners.
- [Browser files and media](docs/browser-media.md): opt-in Blob exports and document-scoped, read-only media URLs with bounded streaming and seeking.
- Docked and floating WebViews retain browser content during resizing, with the native background following the page's opaque HTML/body background color. Windows, macOS and Linux [avoid Canvas initialization for common computed sRGB colors](docs/frontend.md#plain-web-apps). Default Windows title bars and borders retain system styling after undocking.
- New windows prepare their initial rendered page before becoming visible on Windows and macOS. Existing Apps need no startup changes.
- JSFX native-control concealment skips redundant geometry queries for hidden controls and reuses meter handles within each scan on Windows, macOS and Linux.
- Windows, macOS and Linux prepare a shared pool of blank WebViews during host idle time, used by both Apps and JSFX. [Startup diagnostics](docs/devtools.md#startup-timing) report pool usage, JSFX initialization, cached reactivation, host-observer counters and background synchronization separately from native presentation.
- Windows/Linux [DevTools](docs/devtools.md) with a resizable right-hand panel, floating mode and saved layout preferences. Ctrl+Shift+I toggles visibility. Windows offers a borderless embedded panel, native floating window and page context-menu controls.
- macOS [Web Inspector](docs/devtools.md#macos) supports a right-hand embedded panel, native floating window and saved layout preferences. Option+Command+I and the page context menu control the same inspection session.
- WebView, title-bar and Docker tab context menus share docking, reload, DevTools, App folder and close actions. PNG, ICO and SVG window icons from HTML favicons or `reaper.window.setIcon(path)` survive docking and reloads, with visibility applied immediately by `reaper.window.setIconVisible(boolean)`, including the default startup icon. Floating Docker title bars follow the active WebView's icon settings on Windows, macOS and Linux.
- JavaScript and TypeScript templates, Runtime Studio and bilingual documentation.

Windows WebViews respect **Hide pointer while typing** and restore the pointer when it moves over REAPER's native windows. See [pointer behavior](docs/frontend.md#mouse-pointer-while-typing).

For JSFX Web UI in Windows FX chains, use v0.3.8.28 or later to avoid REAPER hangs when switching window focus.

## Download and install

Download a package matching your **REAPER process architecture** from [Releases](https://github.com/zaibuyidao/ReaWebAPI/releases).

| Platform | Architectures | Requirements |
| --- | --- | --- |
| Windows | x64 | Windows 10/11, WebView2 Evergreen, VC++ x64 runtime |
| macOS | ARM64, Intel x64 | macOS 14+ |
| Linux | x64, ARM64 | Ubuntu 24.04 or compatible, WebKitGTK 4.1, X11/XWayland |

REAPER 6.68+ is supported. REAPER 7.80+ provides the complete API catalogue.

| Package | Contents |
| --- | --- |
| Platform ZIP | Extension, SDK, examples and documentation |
| SDK ZIP | TypeScript declarations, templates and API reference |
| ReaPack ZIP | Multi-platform binaries, the complete Demo and ReaPack repository metadata |

Quit REAPER and extract the platform ZIP into its resource directory, available from **Options → Show REAPER resource path in explorer/finder**. Restart REAPER to load the extension.

For manual installation, place the native binary in `UserPlugins/`. On Linux, place the matching `reawebapi-webview-<arch>` helper in the same directory and grant it execute permission.

Run `Scripts/ReaWebAPI/Example/ReaWebAPI_Demo.lua` from the Action List to open the example.

**Preferences > Plug-ins > ReaWebAPI** provides **Use sRGB for WebView rendering on Windows**, unchecked by default. On Windows, checking it requests an sRGB display profile for all ReaWebAPI WebViews after restarting REAPER. **Restore defaults** clears the checkbox. Apply/OK saves changes. The checkbox is disabled on macOS and Linux. See [color profiles](docs/frontend.md#webview-color-profile).

**Preloaded WebView containers** sets the number of unused containers to maintain, from 0 to 32, default 2. Apply/OK takes effect immediately. Zero disables preloading, and Restore defaults selects 2. The pool loads only blank pages and replenishes during idle time. App resources, scripts and JSFX state load when opened. Used containers never return to the pool. If none is ready, opening falls back to normal creation. Preloading uses additional memory and does not eliminate page initialization or first-frame preparation.

## Development

**Diagnostics** on the same Preferences page provides independent App and Runtime debug logging, an exact Target App ID and a log-folder shortcut. Settings apply immediately. See [Runtime Diagnostics](docs/diagnostics.md).

External programs can enable **External Clients** in the same Preferences page and connect to `ws://127.0.0.1:9123`. The server is disabled by default. See the [protocol](docs/external-clients.md) and [standalone browser Demo](web/external-client/README.md).

Lua opens an App with `reaper.ReaWeb_Open(path, instanceKey)`. Pass `debug.getinfo(1, "S").source` as `instanceKey` to reuse the launcher's window. Omitting the key creates a new window. In JavaScript, await `reaper.lifecycle.ready`, then call the REAPER Mirror or Runtime namespaces such as `reaper.window` and `reaper.events`. API calls return Promises, with arguments and results following REAPER's Lua signatures.

Start with `SDK/starter/Open.lua` or use `SDK/modern/` for Vite and TypeScript. `SDK/runtime-demo/Open.lua` opens Runtime Studio.

Set the window name with HTML `<title>` or `document.title`, including Docker labels and single-tab floating Docker captions after restoring a closed docked window. `reaper.window.setTitle(title)` explicitly overrides it. `instanceKey` controls reuse only. See [window titles](docs/runtime-api.md#window-titles).

External HTTP, HTTPS and mailto navigation opens in the system default handler while the App stays open. Open other local HTML with `reaper.window.open(path)`. See [navigation behavior](docs/frontend.md#navigation).

[SDK](runtime/README.md) · [Developer guide](docs/development.md) · [REAPER API](docs/api-reference.md) · [Runtime API](docs/runtime-api.md) · [Lua host API](docs/host-api.md) · [Web runtime](docs/frontend.md) · [DevTools](docs/devtools.md)

All Apps share `ReaWebAPI/WebViewData/`. App identities, private data and window state remain under `ReaWebAPI/Apps/<appId>/`. See the [storage contract](docs/frontend.md#resource-origin-and-storage) for App IDs, origin isolation and native cookie behavior.

## Build

Requires CMake 3.24+, a C++17 toolchain and Python 3.10+. Tests use Node.js. Linux development dependencies are `pkg-config`, `libwebkit2gtk-4.1-dev` and `libx11-dev`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix stage
```

On Windows, use MSVC x64 with `-A x64`. On macOS, select `-DCMAKE_OSX_ARCHITECTURES=arm64` or `x86_64`.

The extension version is defined by `project(ReaWebAPI VERSION …)` in `CMakeLists.txt`, including an optional fourth component. Pushing to the default branch builds all platforms and publishes `v<version>` after the checks pass. Maintain the current changes in `docs/release-notes.md`. Published versions are not overwritten.

[Source layout](docs/source-layout.md) · [API maintenance](api/README.md) · [Release notes](docs/release-notes.md) · [Third-party notices](THIRD_PARTY.md)

Licensed under [LGPL-3.0-or-later](LICENSE.md).

Pure JavaScript apps use the REAPER Mirror and Runtime APIs. For Lua-owned project logic, use the [Lua backend + WebView example](web/lua-backend/README.md). Platform packages install it under `Scripts/ReaWebAPI/Example/lua-backend`, and the SDK includes `lua-backend/`.

EEL2 backends use the same message bridge. The SDK includes a [minimal example](web/eel-backend/README.md) and a [bidirectional track mixer](web/eel-mixer/README.md), installed under `eel-backend/` and `eel-mixer/`. Both examples use `ReaWeb_SetFrame` for state refresh, `0..4` volume gain, and acknowledged slider writes with bounded pending work and final-value protection.

Native Streams share track sampling across analysis views and support `aggregate: true` for synchronized pre-FX source analysis of folders and receive buses: [contract](docs/native-streams.md) · [Demo](web/native-stream/README.md).

Aggregate RMS/LUFS history continues accumulating when routed folders, buses or upstream tracks contain no media.

The built-in Meter provides Sample/True Peak, separate sample/True Peak clip counts, per-channel and global peak maxima, RMS-M/I, LUFS-M/S/I and LRA bounds, with reset and mono calibration. Meter supports 1–32 source channels, exact integer clip counts and selectable integrated history. Analysis is independent of publication rate.

Native [hidden startup](docs/host-api.md#hidden-startup) supports background preloading on Windows, macOS and Linux. See the [example](runtime/hidden-start/README.md).
