# ReaWebAPI

## Changes

- Include the complete `demo/` folder beside `extension/` in the ReaPack ZIP.
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

- ReaPack ZIP 收录完整 `demo/` 文件夹，与 `extension/` 同级。
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
