# TypeScript 模板

[English](README.md) | **简体中文**

配合当前 ReaWebAPI 扩展使用的 Vite 与 TypeScript 模板，开发环境需要 Node.js 22.12+。

将此目录放在 SDK 声明文件旁边。运行 `npm ci` 和 `npm run dev`，再在 REAPER 中运行 `OpenDev.lua`。生成正式构建时，运行 `npm run build`，通过 `Open.lua` 启动。分发时包含 `Open.lua` 和 `dist/`。

示例涵盖轨道 Undo 批处理、文件操作、剪贴板、外链、播放事件、资源和 Worker。音量控件修改选中轨道，保存按钮将数据写入应用文件基准目录下的 `snapshot.json`。

资源路径和开发配置见 [Web 运行环境](../../docs/frontend.zh-CN.md)。
