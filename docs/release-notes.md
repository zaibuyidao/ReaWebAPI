# ReaWebAPI

## Changes

- Add Windows/WebView2 Embedded DevTools with a draggable right-hand divider and Floating mode. Mode switches and hide/show reuse the inspector session without reloading the page. Save mode and panel width through the existing window preferences.
- Move Windows DevTools controls into the WebView context menu alongside Dock/Undock. Show Open/Hide and Float/Embed actions according to current state. Remove the inspector host toolbar and the Demo DevTools button.
- Share state between the context menu, Ctrl+Shift+I and APIs. Improve focus and floating-window stacking, support Per-Monitor V1/V2 DPI hosts, and retain native floating fallback when hosting is unavailable.

## 更新

- Windows/WebView2 新增 Embedded DevTools、可拖动的右侧分隔条和 Floating 模式。模式切换及隐藏、显示复用检查器会话，不重载页面，通过现有窗口偏好保存模式与面板宽度。
- Windows DevTools 控制入口移至 WebView 右键菜单，与 Dock/Undock 并列，按当前状态显示打开、隐藏及浮动、嵌入操作。移除检查器宿主工具栏和 Demo DevTools 按钮。
- 右键菜单、Ctrl+Shift+I 和 API 共用状态管理。改善焦点及浮动窗口层级，支持 Per-Monitor V1/V2 DPI 宿主，无法托管时保留原生浮动回退。
