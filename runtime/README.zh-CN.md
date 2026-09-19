# ReaWebAPI SDK

[English](README.md) | **简体中文**

从 [Releases](https://github.com/zaibuyidao/ReaWebAPI/releases) 下载 `ReaWebAPI-SDK-v<版本>.zip`，也可直接使用源码中的本目录。SDK 不区分处理器架构，原生扩展需另外安装对应版本。

| 文件 | 用途 |
| --- | --- |
| `reaper.d.ts` | 编辑器入口，声明全局 `reaper`、宿主接口、句柄、事件和诊断类型 |
| `reaper-api.generated.d.ts` | 全部 730 项 REAPER 方法、Promise 返回类型、返回值名称和官方链接 |
| `runtime-api.d.ts` | v0.1.7 Runtime namespace types / 命名空间类型 |
| `runtime-demo/` | Runtime Studio: audio, events, lifecycle, windows and theme / 音频与宿主功能示例 |
| `app-manifest.schema.json` | Minimal App manifest schema / 最小应用描述 |
| `starter/` | 可运行的 Lua + HTML/CSS/JavaScript 模板，附带 `checkJs` 配置 |
| `modern/` | Vite/TypeScript 模板：开发服务器、IIFE 构建、资源和 Worker 示例 |
| `web-runtime/` | 无需构建的 Web Runtime v1 检查 App：模块、本地 fetch、存储、Canvas、文件与 Worker |
| `reaper.js`、`reaper-api.generated.js` | 扩展构建使用的桥接源码，不应由网页通过 script 标签加载 |

本 SDK 需要 ReaWebAPI 0.1.7 或更新版本。三个声明文件需放在一起，`reaper.d.ts` 会引用生成的声明。它们提供全局类型，不是 npm 模块，也不参与网页运行。

1. 将整个 SDK 目录复制到开发位置。
2. 在 REAPER 的 Action List 加载并运行 `starter/Open.lua`。
3. 修改 `starter/` 内的四个运行文件，修改后重新打开窗口。
4. 用支持 TypeScript 的编辑器打开模板目录。`jsconfig.json` 会根据上一级声明检查 JavaScript，不生成文件。

整合现有项目时，在 `jsconfig.json` 或 `tsconfig.json` 中包含 `reaper.d.ts`。简单项目也可在入口添加 `/// <reference path="./SDK/reaper.d.ts" />`。TypeScript 必须先编译成浏览器可执行的 JavaScript。

[开发指南](../docs/development.zh-CN.md) · [API 参考](../docs/api-reference.md) · [宿主接口](../docs/host-api.zh-CN.md)

独立 ZIP 还包含 `SDK/api/` 下的机器可读定义和绑定清单、版本及提交记录、校验和。这些是开发资料，不负责注册 API，分发工具时也无需附带。

[v0.1.7 Runtime API](../docs/runtime-api.md) · [Runtime API 中文](../docs/runtime-api.zh-CN.md)

JavaScript 使用 `reaper.window.open(path)` 和 `reaper.lifecycle.ready`，不保留 `reaper.ReaWeb_*`、`reaper.ReaWebOpen` 或 `reaper.ready` 兼容入口。Lua 在网页尚未启动时仍通过 `reaper.ReaWeb_Open(path)` 启动窗口；Lua 原生扩展函数独立于浏览器 SDK。730 项标准 REAPER 镜像名称保持不变。

[Runtime API 完整清单 / Complete inventory](../docs/runtime-api-inventory.md) — 13 implemented namespaces, 67 methods, 1 Promise property.
