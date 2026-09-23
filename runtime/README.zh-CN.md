# ReaWebAPI SDK

[English](README.md) | **简体中文**

配合当前 ReaWebAPI 扩展使用的 TypeScript 类型声明、开发模板和示例。从 [Releases](https://github.com/zaibuyidao/ReaWebAPI/releases) 下载 SDK，或配合已安装的扩展使用本目录。

| 入口 | 用途 |
| --- | --- |
| `reaper.d.ts` | 类型声明入口，引用 Mirror 和 Runtime 声明 |
| `reaper-api.generated.d.ts` | 730 项 REAPER API 签名和返回类型 |
| `runtime-api.d.ts` | Runtime 命名空间和数据类型 |
| `starter/` | Lua、HTML 和 JavaScript 模板，附带编辑器类型检查 |
| `modern/` | Vite 与 TypeScript 模板 |
| `runtime-demo/` | Runtime Studio，演示窗口、事件、音频和应用服务 |
| `web-runtime/` | 模块、资源、存储和浏览器能力检查 |
| `app-manifest.schema.json` | 应用元数据格式 |

将 SDK 复制到工作目录，在 REAPER 的 Action List 运行 `starter/Open.lua`。整合现有项目时，在 `jsconfig.json` 或 `tsconfig.json` 中包含 `reaper.d.ts`，并将三个声明文件保存在同一目录。

修改 `starter/index.html` 的 `<title>` 即可自定义模板的原生窗口名称。动态标题和显式覆盖见[窗口标题](../docs/runtime-api.zh-CN.md#窗口标题)。

扩展自动注入浏览器运行时。Lua 通过 `reaper.ReaWeb_Open(path, instanceKey)` 打开页面。传入 `debug.getinfo(1, "S").source` 作为 `instanceKey` 可复用该启动脚本的窗口，省略 key 则每次创建新窗口。JavaScript 在 `reaper.lifecycle.ready` 完成后调用 Mirror 和 Runtime API。

`reaper.transaction.batch(b => { ... })` 在同步回调内收集已审核的 Mirror 调用，自动处理结果引用和多返回值解构。返回引用、对象或数组可选择结果并推导类型。[批处理契约](../docs/host-api.zh-CN.md#批处理与连续参数) 同时说明原有调用数组形式。

[开发指南](../docs/development.zh-CN.md) · [REAPER API](../docs/api-reference.md) · [Runtime API](../docs/runtime-api.zh-CN.md) · [接口清单](../docs/runtime-api-inventory.md) · [DevTools](../docs/devtools.zh-CN.md)

纯 JavaScript 应用使用 REAPER Mirror 和 Runtime API。需要由 Lua 管理工程逻辑时，参见 [Lua 后端 + WebView 示例](../web/lua-backend/README.md)。平台安装包将其放在 `Scripts/ReaWebAPI/Example/lua-backend`，SDK 提供 `lua-backend/`。
