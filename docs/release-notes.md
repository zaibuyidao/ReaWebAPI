# ReaWebAPI

## English

- Fix `setIconVisible(false)` leaving the default Windows startup icon visible until Dock/Undock. Apply the current icon visibility when showing the window.

## 简体中文

- 修复 Windows 首次启动时调用 `setIconVisible(false)` 后默认图标仍显示、需要 Dock/Undock 才生效的问题。窗口显示时同步应用当前图标可见状态。
