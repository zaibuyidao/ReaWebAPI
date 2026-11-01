# Native source layout / 原生源码结构

v0.1.8 groups native code by responsibility. The root `runtime/` directory remains the JavaScript/TypeScript SDK; `src/runtime/` contains the C++ session implementation and portable host services.

v0.1.8 按职责组织原生代码。根目录 `runtime/` 是浏览器 SDK，`src/runtime/` 是 C++ 会话实现和通用宿主服务，两者不是同一个目录。

```text
src/
├─ core/
│  ├─ core.cpp / core.hpp
│  ├─ reaper_api.cpp
│  ├─ batch.cpp / batch.hpp
│  ├─ native.cpp / native.hpp / native_call.hpp
│  ├─ worker.cpp / worker.hpp
│  └─ file_time.hpp
├─ runtime/
│  ├─ runtime.cpp / runtime.hpp
│  ├─ external_client.cpp
│  ├─ external_transport.cpp / external_transport.hpp
│  ├─ external_settings.cpp / external_settings.hpp
│  ├─ events.cpp
│  ├─ native_monitor.cpp / native_monitor.hpp
│  ├─ host_service.cpp / host_service.hpp
│  ├─ native_stream.cpp / native_stream.hpp
│  ├─ stream_transport.cpp / stream_transport.hpp
│  ├─ native_producers.cpp / native_producers.hpp
│  ├─ audio_analysis.cpp / audio_analysis.hpp
│  ├─ native_tasks.cpp / native_tasks.hpp
│  ├─ service_router.cpp
│  ├─ lifecycle.cpp
│  ├─ services.cpp / services.hpp
│  ├─ jsfx_webui.cpp / jsfx_webui.hpp / jsfx_runtime.cpp
│  ├─ window.cpp
│  ├─ theme.cpp
│  ├─ debug.cpp
│  ├─ fs.cpp / fs.hpp
│  ├─ audio.cpp
│  ├─ drag_drop.cpp
│  ├─ app.cpp
│  ├─ system.cpp
│  └─ transaction.cpp
├─ public/reaweb_service.h / reaweb_stream.h / reaweb_tasks.h
├─ platform/
│  ├─ platform.hpp
│  ├─ windows/platform_win.cpp
│  ├─ macos/platform_mac.mm
│  ├─ linux/platform_linux.cpp
│  ├─ linux/linux_webkit.cpp
│  ├─ linux/linux_channel.hpp
│  ├─ linux/linux_alpha.hpp
│  ├─ linux/gtk_drag.hpp
│  ├─ shared/drag_regions.hpp
│  ├─ shared/transparency.hpp
│  ├─ shared/startup.hpp
│  ├─ shared/jsfx_editor.cpp / jsfx_editor.hpp
│  └─ shared/swell_window.cpp / swell_window.hpp
├─ web/
│  └─ web_resources.cpp / web_resources.hpp
└─ plugin/
   └─ plugin.cpp
```

## App examples / 应用示例

App examples live under `examples/`, separate from the SDK in `runtime/` and the native implementation in `src/`. Launchers use `reawebapi-<example>[-purpose].lua` or `.eel`; the main Demo keeps `ReaWebAPI_Demo.lua`.

应用示例统一放在 `examples/`，与 `runtime/` 中的 SDK 和 `src/` 中的原生实现分开。启动文件采用 `reawebapi-<示例名>[-用途].lua` 或 `.eel`，主 Demo 保留 `ReaWebAPI_Demo.lua`。

| Source directory / 源码目录 | Examples and purpose / 示例与用途 |
| --- | --- |
| `examples/main-demo/` | Main Demo / 完整体验入口 |
| `examples/templates/` | `starter`, `modern`: development templates / 开发模板 |
| `examples/runtime-features/` | `runtime-demo`, `web-runtime`, `hidden-start`: runtime APIs, browser capabilities and hidden startup / 运行时接口、浏览器能力和隐藏启动 |
| `examples/backends/` | `lua-backend`, `eel-backend`, `eel-mixer`: Lua/EEL integration / Lua 与 EEL 后端接入 |
| `examples/integrations/` | `native-service`, `native-stream`, `external-client`: native extensions and external clients / 原生扩展及外部客户端接入 |
| `examples/windows/` | `frameless`, `transparent`: window appearance and behavior / 窗口样式和行为 |

Release installation paths remain unchanged. ReaPack packages only the six main Demo files under `web/`. SDK examples retain their individual directories under `ReaWebAPI/SDK/`; platform packages retain `Scripts/ReaWebAPI/Example/`. The JSFX helper examples remain with the helper under `runtime/jsfx/examples/`.

发布包安装路径保持不变：ReaPack 的 `web/` 只包含主 Demo 的六个文件；SDK 示例保留在 `ReaWebAPI/SDK/` 下各自的目录；平台安装包保留 `Scripts/ReaWebAPI/Example/`。JSFX helper 配套示例继续随 helper 放在 `runtime/jsfx/examples/`。

## Responsibilities / 职责

| Directory | Responsibility / 职责 |
| --- | --- |
| `core/` | Bridge registration and dispatch, 730 Mirror bindings and native marshalling, synchronous batch validation/execution, worker queues, lossless file-time conversion / 桥接与镜像、批处理、工作队列、时间戳基础工具 |
| `runtime/` | Session scheduling and host-call routing in `runtime.cpp`; window ownership/state in `window.cpp`; events, lifecycle, logs, managed Undo and portable App/audio/theme/file/system/drag services in their matching files / 会话与各功能域实现 |
| `platform/` | Abstract host window interface and OS/WebView implementations; SWELL window support shared by macOS and Linux / 平台抽象、系统窗口、WebView 和共享 SWELL 窗口代码 |
| `web/` | Read-only virtual App resources, stable identity and resource-path validation / 虚拟本地资源、稳定身份和路径校验 |
| `plugin/` | REAPER entry point, native host API registration and host callbacks / REAPER 入口、Lua 宿主 API 注册和回调接线 |

These are source responsibility groups, not five independent libraries. `reaweb_core` builds the portable bridge and services, `reaweb_runtime` builds session/window/event/lifecycle orchestration, and `reawebapi` adds the plugin entry and selected OS backend. Runtime tests and the Windows native-drag test link the same `reaweb_runtime` library as the extension. The Linux WebKit helper remains a separate executable.

这五层是源码职责划分，不强行对应五个独立库。`reaweb_core` 构建通用桥接和可移植服务，`reaweb_runtime` 构建会话、窗口、事件和生命周期调度，`reawebapi` 加入插件入口与选定的平台后端。Runtime 测试和 Windows 原生拖放测试与扩展共用同一份 `reaweb_runtime`；Linux WebKit helper 仍是独立进程。

## API boundaries / API 边界

`shared/startup.hpp` owns first-presentation visibility intent and navigation generations on Windows and macOS. Windows keeps the HWND cloaked while WebView2 prepares a capture, using an empty window region when docking before the first frame. macOS waits for a WKWebView snapshot before ordering the window. On these platforms, document loading requests a native capture, independently of the Runtime handshake. Linux retains its v0.3.8.10 display, resize and docking paths without startup snapshots. These are internal hooks and add no public creation options.

Windows uses JPEG for Hosted JSFX readiness captures and PNG for ordinary Apps. The encoded image is never displayed. Capture completion and navigation-generation checks remain prerequisites for automatic first presentation.

`shared/startup.hpp` 管理 Windows 和 macOS 首次展示的可见意图和导航代次。Windows 在 WebView2 准备快照时通过 DWM 隐藏 HWND，首帧前进入 Docker 时改用空窗口区域。macOS 取得 WKWebView 快照后显示窗口。这两个平台在文档加载完成后触发原生快照，与 Runtime 握手独立。Linux 保留 v0.3.8.10 的显示、缩放和停靠流程，不使用启动快照。这些内部钩子不增加公开创建选项。

Windows 的托管 JSFX 就绪截图使用 JPEG，普通 App 使用 PNG。编码图像不用于显示，自动首次展示仍需等待截图完成并通过导航代次校验。

External Clients add an optional localhost protocol inside the same extension. `ReaperApiCore` owns shared native calls, handles and Batch. `Runtime` schedules windows and External Sessions using the same service registry, monitor manager and stream hub. The External transport handles sockets and JSON only. See [External Clients](external-clients.md).

External Client 在同一扩展内增加可选本机协议。`ReaperApiCore` 提供共享原生调用、Handle 和 Batch，`Runtime` 管理窗口与 External Session 的调度，复用同一 Service Registry、Monitor Manager 和 Stream Hub。External Transport 仅处理 socket 与 JSON。参见 [External Client](external-clients.zh-CN.md)。

The public API consists of 730 REAPER Mirror bindings, thirteen Lua `ReaWeb_*` host APIs and fifteen JavaScript Runtime namespaces.

公开接口包含 730 项 REAPER Mirror、13 项 Lua `ReaWeb_*` 宿主 API 和 15 个 JavaScript Runtime 命名空间。

Namespace names do not require one C++ file each. `reaper.dialog.*` wraps the `GetUserFileName` Mirror in `runtime/reaper.js`. Clipboard operations use the common asynchronous dispatcher and the OS implementations in `platform/`.

命名空间不要求与 C++ 文件一一对应。`reaper.dialog.*` 在 `runtime/reaper.js` 包装 `GetUserFileName` Mirror。剪贴板由通用异步分发和 `platform/` 内各系统实现承接。

## Maintenance / 维护

- Use includes relative to the `src/` root, such as `core/core.hpp`, `runtime/runtime.hpp` and `platform/platform.hpp`; do not add every subdirectory to the include search path.
- Register new translation units explicitly in `CMakeLists.txt`. API generation and ABI test generation use the same qualified include paths.
- API Sync reads `src/core/core.cpp`; SDK batch checks read `src/core/batch.hpp`. Keep those source checks synchronized when moving the registries.
- Keep build-generated `api_schema.hpp`, `native_api.cpp` and `bridge_script.hpp` in the build directory.
- Run the existing API, native ABI, Runtime, Python/SDK and packaging checks after structural changes. Keep validation records locally; see the [developer guide](development.md) for build instructions.

Native Streams and platform additions: [English](native-streams.md) · [中文](native-streams.zh-CN.md).

`src/runtime/window.cpp` validates OpenEx options, reuses instances and defers saved Docker attachment during hidden startup. Platform windows receive the initial hidden intent before native presentation. Windows/macOS reuse `shared/startup.hpp` for first-frame gating and explicit Show/Focus.

`src/runtime/window.cpp` 校验 OpenEx 选项、复用实例并延迟隐藏启动的 Docker 接入。平台窗口在原生展示前接收隐藏意图，Windows/macOS 复用 `shared/startup.hpp` 处理首帧门控和主动 Show/Focus。
