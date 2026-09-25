# ReaWebAPI

**English** | [简体中文](README.zh-CN.md)

ReaWebAPI is a native REAPER extension for building tools with HTML, CSS and JavaScript. It provides dockable WebView windows and asynchronous access to REAPER through the `reaper` object.

- 730 standard REAPER 7.80 API bindings with TypeScript declarations.
- Mirror-aware Batch Builder for 456 reviewed APIs, including transport queries and time conversions, with deferred references, tuple destructuring and typed results.
- Lua backend + WebView UI through `ReaWeb_Send`, `ReaWeb_Receive`, `reaper.host.send` and `message` events.
- 14 Runtime namespaces for windows, events, files, native dialogs, drag and drop, audio, Undo and application services.
- Native WebView support for modules, local resources, Workers and persistent App storage.
- Windows/Linux [DevTools](docs/devtools.md) with a resizable right-hand panel, floating mode and saved layout preferences. Ctrl+Shift+I toggles visibility. Windows offers a borderless embedded panel, native floating window and page context-menu controls.
- macOS [Web Inspector](docs/devtools.md#macos) supports a right-hand embedded panel, native floating window and saved layout preferences. Option+Command+I and the page context menu control the same inspection session.
- Dock/Undock from the WebView context menu. PNG, ICO and SVG window icons from HTML favicons or `reaper.window.setIcon(path)` survive docking and reloads, with visibility applied immediately by `reaper.window.setIconVisible(boolean)`, including the default startup icon. Floating Docker title bars follow the active WebView's icon settings on Windows, macOS and Linux.
- JavaScript and TypeScript templates, Runtime Studio and bilingual documentation.

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

## Development

Lua opens an App with `reaper.ReaWeb_Open(path, instanceKey)`. Pass `debug.getinfo(1, "S").source` as `instanceKey` to reuse the launcher's window. Omitting the key creates a new window. In JavaScript, await `reaper.lifecycle.ready`, then call the REAPER Mirror or Runtime namespaces such as `reaper.window` and `reaper.events`. API calls return Promises, with arguments and results following REAPER's Lua signatures.

Start with `SDK/starter/Open.lua` or use `SDK/modern/` for Vite and TypeScript. `SDK/runtime-demo/Open.lua` opens Runtime Studio.

Set the window name with HTML `<title>` or `document.title`, including Docker labels and single-tab floating Docker captions after restoring a closed docked window. `reaper.window.setTitle(title)` explicitly overrides it. `instanceKey` controls reuse only. See [window titles](docs/runtime-api.md#window-titles).

[SDK](runtime/README.md) · [Developer guide](docs/development.md) · [REAPER API](docs/api-reference.md) · [Runtime API](docs/runtime-api.md) · [Lua host API](docs/host-api.md) · [Web runtime](docs/frontend.md) · [DevTools](docs/devtools.md)

All Apps share `ReaWebAPI/WebViewData/`. App identities, private data and window state remain under `ReaWebAPI/Apps/<appId>/`. See the [storage contract](docs/frontend.md#resource-origin-and-storage) for origin isolation and shared cookies.

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
