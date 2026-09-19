# ReaWebAPI 开发文档

[English](README.md) | **简体中文**

用 HTML、CSS 和 JavaScript 编写运行于 REAPER 内的工具。ReaWebAPI 提供可停靠的 WebView 和异步 `reaper` 对象，Lua Action 负责打开页面。

| 入口 | 内容 |
| --- | --- |
| [开发指南](development.zh-CN.md) | 项目结构、编辑器类型提示、调用约定、资源释放、Undo、调试和分发 |
| [REAPER API 参考](api-reference.md) | 全部 730 项 JavaScript 签名、输入类型、返回顺序和官方说明链接 |
| [宿主 API 参考](host-api.zh-CN.md) | 窗口、停靠、事件、批处理、诊断、错误码及 Lua 入口 |
| [SDK](../runtime/README.zh-CN.md) | 文件用途、可直接运行的最小模板和独立下载 |

当前目录对应 REAPER 7.80。较旧宿主只提供它实际具备的函数，依赖特定 API 前应查询能力。Lua 内建函数、`gfx`、`defer`、SWS 和其他第三方扩展 API 不在这 730 项范围内。

安装包和 SDK ZIP 中，源码的 `runtime/` 目录对应 `SDK/`。打包时会自动调整文档链接。

完整 Demo 用于验证 API，SDK 中的 starter 用于开始新工具。扩展会自行注入桥接，页面不要重复加载 `reaper.js` 或生成的方法列表。

维护扩展本身时，请阅读仓库中的 [API Sync 维护说明](https://github.com/zaibuyidao/ReaWebAPI/tree/main/api)。逐项 API 参考和编辑器声明一起生成，构建前的 `python -m tools.api_sync verify` 会检查是否过期。

- [Frontend / 前端资源约定](frontend.md)
- [前端资源约定（中文）](frontend.zh-CN.md)
- [v0.1.6 release notes / 发布说明](release-notes.md)
