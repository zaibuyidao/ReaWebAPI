# ReaWebAPI SDK

[English](README.md) | **简体中文**

配合当前 ReaWebAPI 扩展使用的 TypeScript 类型声明、开发模板和示例。从 [Releases](https://github.com/zaibuyidao/ReaWebAPI/releases) 下载 SDK，或配合已安装的扩展使用本目录。

| 入口 | 用途 |
| --- | --- |
| `reaper.d.ts` | 类型声明入口，引用 Mirror 和 Runtime 声明 |
| `reaper-api.generated.d.ts` | 730 项 REAPER API 签名和返回类型 |
| `runtime-api.d.ts` | Runtime 命名空间和数据类型 |
| `jsfx-webui.mjs` | [JSFX Web UI](../docs/jsfx-webui.zh-CN.md) 的共享 `editor` 尺寸、`params` 参数绑定、Promise 合并写入、参数组手势、DSP 流、状态、命令和二进制资源。 |
| `browser-files.mjs` | 可选的 Blob/data URL 原生保存模块，附带 `browser-files.d.mts` 类型声明。参见[浏览器媒体接入](../docs/browser-media.md)。 |
| [Native Service Demo](../web/native-service/README.md) | 原生事件、服务与 Lua 共存。SDK 包含 `native/reaweb_service.h` 和 `native/test_extension.cpp`，参见 [接入文档](../docs/native-services.zh-CN.md)。 |
| `starter/` | Lua、HTML 和 JavaScript 模板，附带编辑器类型检查 |
| `modern/` | Vite 与 TypeScript 模板 |
| `runtime-demo/` | Runtime Studio，演示窗口、事件、音频和应用服务 |
| `web-runtime/` | 模块、资源、存储和浏览器能力检查 |
| `app-manifest.schema.json` | 应用元数据格式 |

将 SDK 复制到工作目录，在 REAPER 的 Action List 运行 `starter/Open.lua`。整合现有项目时，在 `jsconfig.json` 或 `tsconfig.json` 中包含 `reaper.d.ts`，并将三个声明文件保存在同一目录。

修改 `starter/index.html` 的 `<title>` 即可自定义模板的原生窗口名称。动态标题和显式覆盖见[窗口标题](../docs/runtime-api.zh-CN.md#窗口标题)。

扩展自动注入浏览器运行时。Lua 通过 `reaper.ReaWeb_Open(path, instanceKey)` 打开页面。传入 `debug.getinfo(1, "S").source` 作为 `instanceKey` 可复用该启动脚本的窗口，省略 key 则每次创建新窗口，并要求定义 `app.json.id`。未定义该字段时使用启动脚本文件名生成回退 App ID。生产页面地址为 `reaweb://<appId>/...`。复制模板创建新 App 时应设置唯一的 ID。JavaScript 在 `reaper.lifecycle.ready` 完成后调用 Mirror 和 Runtime API。

`reaper.transaction.batch(b => { ... })` 在同步回调内收集已审核的 Mirror 调用，自动处理结果引用和多返回值解构。返回引用、对象或数组可选择结果并推导类型。[批处理契约](../docs/host-api.zh-CN.md#批处理与连续参数) 同时说明原有调用数组形式。

[开发指南](../docs/development.zh-CN.md) · [REAPER API](../docs/api-reference.md) · [Runtime API](../docs/runtime-api.zh-CN.md) · [接口清单](../docs/runtime-api-inventory.md) · [DevTools](../docs/devtools.zh-CN.md)

纯 JavaScript 应用使用 REAPER Mirror 和 Runtime API。需要由 Lua 管理工程逻辑时，参见 [Lua 后端 + WebView 示例](../web/lua-backend/README.md)。平台安装包将其放在 `Scripts/ReaWebAPI/Example/lua-backend`，SDK 提供 `lua-backend/`。

EEL2 后端使用同一消息桥。SDK 提供[最简示例](../web/eel-backend/README.zh-CN.md)和[双向轨道混音示例](../web/eel-mixer/README.zh-CN.md)，分别安装于 `eel-backend/` 和 `eel-mixer/`。

[Native Stream 示例](../web/native-stream/README.md) · [Native Stream ABI 与平台契约](../docs/native-streams.zh-CN.md)。SDK 包含 `native/reaweb_stream.h`, `native/reaweb_tasks.h` 和 `native/stream_extension.cpp`。

[Hidden startup / 隐藏启动示例](hidden-start/README.md)

[无边框窗口示例](../web/frameless/README.md)提供可缩放和固定尺寸 Lua 启动脚本及原生 HTML 拖动区。SDK 安装包将此示例放在 `frameless/` 目录。

[透明窗口示例](../web/transparent/README.md)演示桌面 Alpha、半透明面板、圆角和阴影，SDK 将其放在 `transparent/`。

JSFX helper 在 Windows、macOS 和 Linux 共用初始化路径。能力握手后并发注册订阅，再并发读取初始尺寸与参数。初始读取遇到 `STALE_SESSION` 时刷新能力并最多重试两次快照，不重试写入。请随 UI 分发更新后的 `jsfx-webui.mjs`。参见[连接语义](../docs/jsfx-webui.zh-CN.md#参数服务)与[启动计时](../docs/devtools.zh-CN.md#启动计时)。
