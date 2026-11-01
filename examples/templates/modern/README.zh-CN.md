# TypeScript 模板

[English](README.md) | **简体中文**

配合当前 ReaWebAPI 扩展使用的 Vite 与 TypeScript 模板，开发环境需要 Node.js 22.12+。

仓库源码中的模板从 `../../../runtime/` 读取类型声明；SDK 安装包中的模板仍放在声明文件旁边，包内类型配置使用 `../`。运行 `npm ci` 和 `npm run dev`，再在 REAPER 中运行 `reawebapi-modern-dev.lua`。生成正式构建时，运行 `npm run build`，通过 `reawebapi-modern.lua` 启动。分发时包含 `reawebapi-modern.lua` 和 `dist/`。

使用此模板创建 App 时，在 `public/app.json` 中设置唯一的 `id`。Vite 将清单复制到 `dist/`，用于确定生产 App 的虚拟 origin。

示例涵盖 Mirror-aware Undo 批处理、文件操作、剪贴板、外链、播放事件、资源和 Worker。音量控件修改选中轨道，并通过 Builder 返回轨道名和音量。Read transport 按钮通过一次批量 RPC 读取播放位置、状态和速度，无需选中轨道。保存按钮将数据写入应用文件基准目录下的 `snapshot.json`。

资源路径和开发配置见 [Web 运行环境](../../../docs/frontend.zh-CN.md)。
