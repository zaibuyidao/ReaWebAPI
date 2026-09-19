# ReaWebAPI v0.1.7

## Changes

- Fix thirteen JavaScript Runtime namespace boundaries: `reaper.window`, `reaper.theme`, `reaper.dialog`, `reaper.events`, `reaper.lifecycle`, `reaper.debug`, `reaper.fs`, `reaper.audio`, `reaper.clipboard`, `reaper.dragDrop`, `reaper.app`, `reaper.system`, `reaper.transaction`. Move existing batching/Undo to transaction, clipboard text to clipboard, and capability discovery/external links to system. All thirteen namespaces now provide implemented members (67 methods and one ready property). Add App metadata/data paths, native file/text drag/drop, system platform/architecture/file-manager integration and events.off; remove debug.info in favor of log. No compatibility aliases. Lua bootstrap remains ReaWeb_Open.

- Keep all 730 REAPER 7.80 standard API bindings, original names, typed handles, Promise semantics and Lua-order return values. Preserve Web Runtime v1, batches, managed Undo, files, storage and the Windows CRLF no-op fix.
- Add complete ambient TypeScript definitions for thirteen Runtime namespaces alongside the generated mirror declarations.
- Add `reaper.lifecycle.on`: `before-close`, `before-reload`, `cleanup` and the `destroy` alias. Await asynchronous state saving with a 2-second ceiling, then finish native cleanup; existing Apps without listeners retain their close behavior.
- Add 12 event names covering track additions/deletions/selection, Item/Take invalidation, playback state, tempo, markers, FX, project load/save and theme. Native callbacks record atomic revisions; state reads and dispatch stay on REAPER's main thread, with subscriptions and incremental scans limiting work.
- Add `window` size/position queries and setters, show/hide, title, focus, docking and reload. Floating geometry is distinct from REAPER-controlled Docker layout on all three backends.
- Add `reaper.dialog.openFile/saveFile/selectFolder` through REAPER's native dialogs, with filters and consistent `null` cancellation.
- Add basic theme colors, `--reaper-*` CSS variables, theme change subscriptions and reversible `reaper.theme.apply()`.
- Add explicit runtime logging, circular-object inspection, automatic uncaught JavaScript/rejection reporting and bounded native-error diagnostics.
- Implement `reaper.audio.getFileInfo`, `getWaveform` and `getTrackMeter`, including metadata, per-channel min/max arrays, linear/dB meter values, typed-handle validation and owned PCM-source cleanup. Waveform construction advances across ticks and rejects unavailable data explicitly.
- Add a minimal App manifest schema and read-only entry validator. Include Runtime Studio, a runnable no-build example, bilingual Runtime documentation and all new declarations/tools in SDK and platform packages.

## 中文说明

JavaScript Runtime 固定为 13 个命名空间（均已有具体能力，共 67 个方法和 1 个 Promise 属性），没有扁平兼容别名。批处理及 Undo 归 `reaper.transaction`，文件归 `reaper.fs`，剪贴板归 `reaper.clipboard`，能力查询与外部链接归 `reaper.system`，连续混音控制归 `reaper.audio`，诊断和缓冲区设置归 `reaper.debug`。Lua 原生启动器继续使用 `reaper.ReaWeb_Open`。见 [完整 API 清单](runtime-api-inventory.md)。

v0.1.7 补齐开发者 Runtime 层：完整 TypeScript 定义、带超时的生命周期通知、12 个新增事件、统一窗口和原生对话框、基础主题、调试日志，以及可工作的音频文件信息、分声道波形和轨道电平接口。原有 730 项 REAPER 镜像及 Web Runtime v1 保持兼容。

新增 Runtime Studio 示例和最小 Manifest 校验器。没有引入第三方 API、自定义 Lua RPC、应用管理/安装/更新系统、权限执行机制或高级音频分析。项目许可证仍未添加，依赖声明保留。

接口、数据结构与使用示例见 [Runtime API 中文](runtime-api.zh-CN.md) / [English](runtime-api.md)。

## Behavior and limits

- Content/FX/marker events invalidate caches; they are not an exact edit journal. Project-save notification observes serialization plus a changed project-file timestamp/path and a clean project, rather than promising exactly-once filesystem completion.
- Lifecycle waits are bounded. Process termination, crashes and forced destruction cannot guarantee asynchronous saving. PCM sources and managed Undo retain native fallback cleanup.
- Audio files support 1–32 channels and up to 8192 requested waveform frames per channel. At most eight audio jobs are pending per runtime; waveform timeout is 120 seconds. Individual REAPER decoder/peak calls run on the main thread and cannot be preempted. REAPER may create its own `.reapeaks` files; there is no additional ReaWebAPI waveform cache.
- Meter values are snapshots, not a real-time stream or RMS/LUFS analysis. DSP, spectrum, loudness and batch analysis remain outside this version.
- Native window dimensions use desktop/backend units, not CSS pixels. Docked size/position setters reject with `WINDOW_DOCKED`.
- Apps remain trusted native-capability clients. The manifest is a development check, not an installation or permissions system; future considerations are recorded in the [permission design](permission-design.md).

## Release acceptance

Automated coverage checks all 730 typed mirror mappings, new native and JavaScript behavior, cleanup timeouts, audio data/resource ownership, TypeScript, manifest validation, SDK packaging and source-checkout reproducibility. Browser conformance checks preserve the existing Web Runtime contract.

Actual REAPER acceptance is still required on each distributed platform and architecture, particularly native file dialogs, decoder formats, theme changes, track/Item/FX edits and save/load notifications. Windows/Linux builds and mock-host/browser tests do not substitute for macOS/ARM64 CI or real-project Undo/redo acceptance.
