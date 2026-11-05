# 覆盖层与原生鼠标穿透

[English](window-overlay.md)

v0.3.9.24 为独立浮动 App 提供覆盖层和鼠标策略。嵌入 FX 的 JSFX WebUI 保持原有行为。

```js
const id = await reaper.window.open('overlay/index.html', {
  frameless: true, transparent: true,
  layer: 'overlay', mouseMode: 'regions',
  mouseRegions: [{x: 20, y: 20, width: 80, height: 80}]
});
// 以下接口操作当前窗口。
await reaper.window.setMouseRegions([{x: 20, y: 20, width: 80, height: 80}]);
await reaper.window.setMouseMode('regions');
```

| 选项 | 默认值 | 行为 |
| --- | --- | --- |
| `layer` | `normal` | `overlay` 位于 REAPER 主界面之上、浮动窗口之下。仅创建时设置，不全局置顶。 |
| `mouseMode` | `normal` | `normal` 接收客户区输入，`passthrough` 全部穿透，`regions` 仅区域并集接收输入。与层级和视觉透明独立。 |
| `mouseRegions` | `[]` | 相对网页视口左上角的 CSS 像素矩形。`regions` 模式下空列表表示客户区全部穿透。 |

矩形的 `x`、`y`、`width`、`height` 必须为有限数值，宽高必须大于零。宿主裁剪视口外部分。非法列表拒绝整次更新。`setMouseRegions` 原子替换区域列表。两个设置接口成功接受后返回 `true`，失败按现有错误契约 reject。鼠标手势保留原输入目标，待释放后应用新策略。系统标题栏和边框保持原生行为。

宿主负责 DPI 和 WebView 缩放换算。App 在 DOM 布局改变后，通过 `getBoundingClientRect()` 和布局观察提交新区域。宿主不推断 DOM／SVG 形状。鼠标策略不改变 `setKeyboardCapture`、`setDawKeyboard`、文本输入和 IME 规则。全穿透不会自动释放已取得的键盘焦点。

## Lua 接口

```lua
local id = reaper.ReaWeb_OpenEx(path,
  '{"layer":"overlay","mouseMode":"regions","frameless":true,"transparent":true}')
local ok = reaper.ReaWeb_SetMouseRegions(id, '[{"x":20,"y":20,"width":80,"height":80}]')
ok = reaper.ReaWeb_SetMouseMode(id, 'normal')
if not ok then reaper.ShowConsoleMsg(reaper.ReaWeb_GetLastError() .. '\n') end
-- 全穿透状态下仍可调用 ReaWeb_Close(id)。
```

Lua 设置接口返回 Boolean，诊断由 `ReaWeb_GetLastError()` 提供。无效或已关闭 ID 在 JS 中以 `WINDOW_CLOSED` reject，在 Lua 中返回 false。实例复用保留原层级、模式和区域，忽略新的创建选项。

## 生命周期与能力

`getState()` 和 `windowstatechange` 增加 `layer`、`layerActive`、`mouseMode`、`mouseModeActive`。Active 字段表示请求策略已在原生层建立，与可见性独立。初始化或等待手势结束时可为 false。重载保留层级和模式，清空旧文档区域。关闭后重新创建采用新选项，不跨 REAPER 启动持久化。

覆盖层或非 normal 鼠标策略以 `DOCK_UNAVAILABLE` 拒绝停靠。创建时暂时绕过已保存的停靠位置，同时保留原停靠偏好。停靠窗口需要先显式取消停靠，再修改鼠标模式。嵌入式 JSFX 编辑器以 `WINDOW_HOSTED` 拒绝策略设置。

`capabilities.runtime.window.overlay` 分别提供 `layer`、`passthrough`、`regions`、`combined` 的 `supported` 与 `reason`，并报告 `floatingOnly`、`backend`。可用性不代表已完成实机验收。

| 平台 | 原生实现及条件 |
| --- | --- |
| Windows | Win32 窗口事件维护层级，在分发鼠标前调整 HWND／WebView2 命中策略。覆盖层需要 REAPER 主窗口作为所有者。 |
| macOS | AppKit 层级与会话鼠标事件 tap。非 normal 鼠标模式需要 REAPER 的辅助功能权限，不自动弹出授权请求。 |
| Linux | 原生 X11、X Shape 1.1、XInput 2.1、支持 `_NET_RESTACK_WINDOW` 的窗口管理器和 REAPER 的 GLib 事件循环。输入形状与 WebKitGTK 画面独立。上述策略不支持 XWayland／Wayland。输入形状视口的每个轴必须小于 32768 原生像素。 |

`INVALID_ARGUMENT` 表示参数错误。`LAYER_UNAVAILABLE`、`MOUSE_MODE_UNAVAILABLE` 表示原生环境不支持。`LAYER_FAILED`、`MOUSE_MODE_FAILED` 表示原生设置或更新失败。运行时更新失败保留此前请求状态。异步初始化失败沿用现有窗口清理和诊断机制。

[覆盖层示例](../examples/windows/overlay/README.md)提供交互控件、DOM 区域更新及窗口外恢复／关闭的 Lua 操作。
