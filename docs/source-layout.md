# Native source layout / 原生源码结构

v0.1.8 groups native code by responsibility. The root `runtime/` directory remains the JavaScript/TypeScript SDK; `src/runtime/` contains the C++ session implementation and portable host services.

v0.1.8 按职责组织原生代码。根目录 `runtime/` 是浏览器 SDK，`src/runtime/` 是 C++ 会话实现和通用宿主服务，两者不是同一个目录。

```text
src/
├─ core/
│  ├─ core.cpp / core.hpp
│  ├─ batch.cpp / batch.hpp
│  ├─ native.cpp / native.hpp / native_call.hpp
│  ├─ worker.cpp / worker.hpp
│  └─ file_time.hpp
├─ runtime/
│  ├─ runtime.cpp / runtime.hpp
│  ├─ events.cpp
│  ├─ native_monitor.cpp / native_monitor.hpp
│  ├─ host_service.cpp / host_service.hpp
│  ├─ service_router.cpp
│  ├─ lifecycle.cpp
│  ├─ services.cpp / services.hpp
│  ├─ window.cpp
│  ├─ theme.cpp
│  ├─ debug.cpp
│  ├─ fs.cpp / fs.hpp
│  ├─ audio.cpp
│  ├─ drag_drop.cpp
│  ├─ app.cpp
│  ├─ system.cpp
│  └─ transaction.cpp
├─ public/reaweb_service.h
├─ platform/
│  ├─ platform.hpp
│  ├─ windows/platform_win.cpp
│  ├─ macos/platform_mac.mm
│  ├─ linux/platform_linux.cpp
│  ├─ linux/linux_webkit.cpp
│  ├─ linux/linux_channel.hpp
│  ├─ linux/gtk_drag.hpp
│  └─ shared/swell_window.cpp / swell_window.hpp
├─ web/
│  └─ web_resources.cpp / web_resources.hpp
└─ plugin/
   └─ plugin.cpp
```

## Responsibilities / 职责

| Directory | Responsibility / 职责 |
| --- | --- |
| `core/` | Bridge registration and dispatch, 730 Mirror bindings and native marshalling, synchronous batch validation/execution, worker queues, lossless file-time conversion / 桥接与镜像、批处理、工作队列、时间戳基础工具 |
| `runtime/` | Session scheduling and host-call routing in `runtime.cpp`; window ownership/state in `window.cpp`; events, lifecycle, logs, managed Undo and portable App/audio/theme/file/system/drag services in their matching files / 会话与各功能域实现 |
| `platform/` | Abstract host window interface and OS/WebView implementations; SWELL window support shared by macOS and Linux / 平台抽象、系统窗口、WebView 和共享 SWELL 窗口代码 |
| `web/` | Read-only local App resource server, stable origins and resource-path validation / 本地资源服务、稳定来源和路径校验 |
| `plugin/` | REAPER entry point, native host API registration and host callbacks / REAPER 入口、Lua 宿主 API 注册和回调接线 |

These are source responsibility groups, not five independent libraries. `reaweb_core` builds the portable bridge and services, `reaweb_runtime` builds session/window/event/lifecycle orchestration, and `reawebapi` adds the plugin entry and selected OS backend. Runtime tests and the Windows native-drag test link the same `reaweb_runtime` library as the extension. The Linux WebKit helper remains a separate executable.

这五层是源码职责划分，不强行对应五个独立库。`reaweb_core` 构建通用桥接和可移植服务，`reaweb_runtime` 构建会话、窗口、事件和生命周期调度，`reawebapi` 加入插件入口与选定的平台后端。Runtime 测试和 Windows 原生拖放测试与扩展共用同一份 `reaweb_runtime`；Linux WebKit helper 仍是独立进程。

## API boundaries / API 边界

The public API consists of 730 REAPER Mirror bindings, thirteen Lua `ReaWeb_*` host APIs and fourteen JavaScript Runtime namespaces.

公开接口包含 730 项 REAPER Mirror、13 项 Lua `ReaWeb_*` 宿主 API 和 14 个 JavaScript Runtime 命名空间。

Namespace names do not require one C++ file each. `reaper.dialog.*` wraps the `GetUserFileName` Mirror in `runtime/reaper.js`. Clipboard operations use the common asynchronous dispatcher and the OS implementations in `platform/`.

命名空间不要求与 C++ 文件一一对应。`reaper.dialog.*` 在 `runtime/reaper.js` 包装 `GetUserFileName` Mirror。剪贴板由通用异步分发和 `platform/` 内各系统实现承接。

## Maintenance / 维护

- Use includes relative to the `src/` root, such as `core/core.hpp`, `runtime/runtime.hpp` and `platform/platform.hpp`; do not add every subdirectory to the include search path.
- Register new translation units explicitly in `CMakeLists.txt`. API generation and ABI test generation use the same qualified include paths.
- API Sync reads `src/core/core.cpp`; SDK batch checks read `src/core/batch.hpp`. Keep those source checks synchronized when moving the registries.
- Keep build-generated `api_schema.hpp`, `native_api.cpp` and `bridge_script.hpp` in the build directory.
- Run the existing API, native ABI, Runtime, Python/SDK and packaging checks after structural changes. See `docs/VALIDATION.md` in the repository and the [developer guide](development.md).
