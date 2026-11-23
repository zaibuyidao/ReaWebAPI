# ReaWebAPI

**English** | [简体中文](README.zh-CN.md)

ReaWebAPI is a native REAPER extension for building dockable tools and JSFX editors with HTML, CSS and JavaScript. Pages run inside REAPER and use the asynchronous `window.reaper` API to interact with the host.

[API reference](https://zaibuyidao.github.io/ReaWebAPI/reawebapihelp.html) · [Developer guide](docs/development.md) · [Releases](https://github.com/zaibuyidao/ReaWebAPI/releases)

## Capabilities

- **Apps and windows.** Open local pages in floating or docked windows. Apps can use native file dialogs, read and write files, and retain settings across sessions.
- **Project control.** Read transport state, observe track and FX changes, and update track controls or FX parameters. Explicit edit gestures can group changes into a single undo step.
- **Audio and video.** Consume native audio streams for waveforms, spectrum analysis and loudness metering. Project video preview depends on the host and platform backend.
- **JSFX editors.** Associate a web UI with a JSFX instance to read and edit its parameters. Effects that implement the optional EEL SDK can also expose DSP data, commands and binary resources.

## Architecture

A ReaScript launcher opens a local HTML entry point with `ReaWeb_Open`. The extension owns the window and injects the JavaScript runtime. Once `reaper.lifecycle.ready` resolves, the page can call the runtime directly. A tool that keeps its project logic in Lua or EEL2 can exchange messages with the page instead. In a JSFX editor, REAPER continues to own the DSP and parameter state.

Other native extensions can register named services and binary streams through the C/C++ SDK. An optional authenticated localhost WebSocket server exposes services, events and streams to external clients. It is disabled by default.

## Installation

Requires REAPER 6.68 or later. Choose a package that matches the **REAPER process architecture**.

| Platform | Architecture | Runtime requirements |
| --- | --- | --- |
| Windows | x64 | Windows 10 or 11, WebView2 Evergreen, Visual C++ x64 runtime |
| macOS | Apple Silicon (ARM64), Intel (x86_64) | macOS 13 or later, system WKWebView |
| Linux | x86_64, ARM64 | Ubuntu 24.04 or compatible, WebKitGTK 4.1, X11 or XWayland |

- **ReaPack.** Import the [zaibuyidao repository index](https://github.com/zaibuyidao/ReaScripts/raw/master/index.xml), install `ReaWebAPI: Web Runtime for REAPER` and restart REAPER. The package registers the demo and the offline API reference in the Action List.
- **Platform ZIP.** Open **Options → Show REAPER resource path in explorer/finder**, then quit REAPER. Extract the entire platform ZIP from Releases into that directory and restart. On Linux, keep the bundled helper executable. The ZIP includes the extension, examples, SDK and offline reference.

## Run your first app

1. Open REAPER’s **Action List**. After a ReaPack install, search for `ReaWebAPI_Demo`. With a platform ZIP, load `Scripts/ReaWebAPI/Example/ReaWebAPI_Demo.lua` from the resource directory. Run the action to open the demo.
2. To build your own tool, copy `ReaWebAPI/SDK/` from the platform ZIP to a writable location, or use the standalone SDK from Releases. Load and run `starter/reawebapi-starter.lua` from that copy. Select a track in REAPER to see its name in the starter window.
3. Edit `starter/index.html` and `starter/app.js`, then reload the page. The JavaScript starter needs no build step. For Vite and TypeScript, use the `modern/` template.

For offline API lookup, open `ReaWebAPI/docs/reawebapihelp.html` from the platform or SDK ZIP. With ReaPack, run `ReaWebAPI - API Reference` from the Action List.

## Documentation

| Topic | Guides |
| --- | --- |
| App development | [SDK and templates](runtime/README.md) · [Runtime API](docs/runtime-api.md) · [ReaScript host API](docs/host-api.md) · [WebView resources and storage](docs/frontend.md) |
| Host data and media | [Realtime subscriptions](docs/realtime.md) · [Native streams](docs/native-streams.md) · [Project video](docs/video-preview.md) |
| Integration | [JSFX Web UI](docs/jsfx-webui.md) · [Native services](docs/native-services.md) · [External clients](docs/external-clients.md) |
| Debugging | [WebView developer tools](docs/devtools.md) · [Runtime diagnostics](docs/diagnostics.md) |

[All documentation](docs/README.md) · [Examples](examples/) · [Source layout](docs/source-layout.md)

## Build from source

Requires CMake 3.24+, a C++17 toolchain, Python 3.10+ and Node.js 22.12+. Use MSVC x64 on Windows. On Linux, install the WebKitGTK 4.1 and X11 development packages listed in the [build workflow](.github/workflows/build.yml).

```sh
npm ci --prefix examples/templates/modern
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix stage
```

## License

[LGPL-3.0-or-later](LICENSE.md) · [Third-party notices](THIRD_PARTY.md)
