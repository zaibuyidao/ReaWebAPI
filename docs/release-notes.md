# ReaWebAPI

## Changes

- Synchronize HTML PNG, ICO and SVG favicons to the native window, with in-memory rendering at the current DPI. Keep `reaper.window.setIcon(path)` as an explicit override. The Demo declares `logo.svg` in HTML.
- Retain window icons and visibility in Runtime state across docking, native window recreation and reloads. Add `reaper.window.setIconVisible(boolean)` to hide the title-bar icon and its space without clearing the icon. Linux decoration support depends on the window manager.
- Move Dock/Undock to the top of the WebView context menu on all platforms and remove the host toolbar. Preserve page context-menu handling, the Windows title-bar entry and the existing docking implementation.
- Add Ctrl+Shift+I to show/hide DevTools on Windows/Linux and a Safari Inspector guide on macOS.
- Handle Ctrl+Shift+I inside Windows DevTools without opening additional inspectors, and preserve the latest visibility request during asynchronous opening on Linux.
- Add a resizable right-hand DevTools panel on Linux with session-preserving float/dock switching and saved mode/width preferences. Windows uses native floating DevTools.
- Reduce small API round-trip and state-notification latency, and start Demo/Starter track reads before window setup.
- Include the complete `web/` Demo folder beside `extension/` in the ReaPack ZIP, with matching download paths, generated installation entries and Action List registration for the launcher.
- Add a visible Demo debug log with track snapshots, confirmed Pan changes, Copy/Clear and optional REAPER console output.
- Handle DevTools workspace discovery without CSP or missing-resource errors in the Demo.
- Preserve the complete CMake version, including the fourth component, across installation bundles, SDK, ReaPack and GitHub Releases.
- Prioritize native track-selection notifications and project state updates.
- Refresh Demo track names and pan without waiting for color conversion.
- Streamline documentation and examples around the current release.
- License ReaWebAPI under LGPL-3.0-or-later and include the notices in SDK and platform packages.

- Organize native sources into core, runtime, platform, web and plugin modules.
- Split Runtime implementations by responsibility and share the session library between the extension and tests.
- Update build configuration, code generators and documentation for the new structure.

## 更新

- HTML favicon 自动同步到原生窗口，支持 PNG、ICO、SVG，按当前 DPI 在内存中渲染。保留 `reaper.window.setIcon(path)` 显式覆盖，Demo 在 HTML 中声明 `logo.svg`。
- Runtime 保存窗口图标和显示状态，在停靠切换、原生窗口重建和重载后恢复。新增 `reaper.window.setIconVisible(boolean)`，隐藏标题栏图标及占位并保留图标数据。Linux 装饰支持取决于窗口管理器。
- 各平台将 Dock/Undock 移至 WebView 右键菜单顶部，移除宿主工具栏。保留网页上下文菜单行为、Windows 标题栏入口及现有停靠实现。
- Windows/Linux 支持 Ctrl+Shift+I 显示或隐藏 DevTools，macOS 使用该快捷键切换 Safari 检查器指引。
- 修复 Windows DevTools 内按 Ctrl+Shift+I 重复打开检查器，以及 Linux 异步打开过程中取消显示后仍弹出的问题。
- Linux 新增可调整宽度的右侧 DevTools 面板，嵌入与浮动切换保留会话，并保存模式和宽度偏好。Windows 使用原生浮动 DevTools。
- 缩短小型 API 调用和状态通知的等待时间，Demo 与 Starter 优先读取轨道信息。
- ReaPack ZIP 收录完整 `web/` 示例文件夹，与 `extension/` 同级，统一下载路径，自动生成资源安装条目并将启动器注册到 Action List。
- Demo 增加调试日志，显示轨道快照和 Pan 实际变化，支持复制、清空及同步输出到 REAPER 控制台。
- 完善开发者工具的工作区探测处理，消除 Demo 中对应的 CSP 和资源缺失报错。
- 安装包、SDK、ReaPack 和 GitHub Release 统一使用 CMake 完整版本号，支持第四段修订号。
- 优先处理原生轨道选择通知与工程状态更新。
- Demo 轨道名称和 Pan 随查询结果更新，颜色转换独立完成。
- 精简文档和示例说明，统一描述当前版本。
- 采用 LGPL-3.0-or-later 许可，SDK 和平台安装包附带许可声明。

- 将原生源码整理为 core、runtime、platform、web 和 plugin 五个模块。
- 按职责拆分 Runtime 实现，扩展与测试共用会话运行库。
- 同步更新构建配置、代码生成工具和文档。
