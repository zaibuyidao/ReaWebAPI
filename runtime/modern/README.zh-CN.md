# 现代 TypeScript 模板

[English](README.md) | **简体中文**

需要 ReaWebAPI v0.1.6，构建需要 Node.js 22.12+，建议使用 Node 24。

1. 将此目录放在 SDK 类型声明文件旁边。
2. 在此目录运行 `npm ci`、`npm run dev`，然后在 REAPER 运行 `OpenDev.lua`。
3. 运行 `npm run build`，再运行 `Open.lua` 验证生产模式本地页面。
4. 一起分发 `Open.lua` 与 `dist/`，最终用户不需要 Node.js。

示例包含轨道 Undo 批处理、异步文件、剪贴板、外链、播放事件、资源读取和内联 Worker。音量按钮会修改选中轨道；保存按钮明确覆盖页面文件基准目录下的 snapshot.json。开发/生产路径差异与 WebView 限制见[资源约定](../../docs/frontend.zh-CN.md)。
