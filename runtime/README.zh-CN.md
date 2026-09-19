# ReaWebAPI SDK

[English](README.md) | **简体中文**

适用于 ReaWebAPI 0.1.7+ 的 TypeScript 类型声明、开发模板和示例。从 [Releases](https://github.com/zaibuyidao/ReaWebAPI/releases) 下载 SDK，或配合已安装的扩展使用本目录。

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

扩展自动注入浏览器运行时。Lua 通过 `reaper.ReaWeb_Open(path)` 打开页面，JavaScript 在 `reaper.lifecycle.ready` 完成后调用 Mirror 和 Runtime API。

[开发指南](../docs/development.zh-CN.md) · [REAPER API](../docs/api-reference.md) · [Runtime API](../docs/runtime-api.zh-CN.md) · [接口清单](../docs/runtime-api-inventory.md)
