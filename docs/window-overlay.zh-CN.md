# 覆盖层与原生鼠标穿透

[English](window-overlay.md)

覆盖层和鼠标策略适用于独立浮动 App。嵌入 FX 的 JSFX WebUI 保持原有行为。

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

`getState()` 和 `windowstatechange` 包含 `layer`、`layerActive`、`mouseMode`、`mouseModeActive`。Active 字段表示请求策略已在原生层建立，与可见性独立。初始化或等待手势结束时可为 false。重载保留层级和模式，清空旧文档区域。关闭后重新创建采用新选项，不跨 REAPER 启动持久化。

覆盖层或非 normal 鼠标策略以 `DOCK_UNAVAILABLE` 拒绝停靠。创建时暂时绕过已保存的停靠位置，同时保留原停靠偏好。停靠窗口需要先显式取消停靠，再修改鼠标模式。嵌入式 JSFX 编辑器以 `WINDOW_HOSTED` 拒绝策略设置。

`capabilities.runtime.window.overlay` 分别提供 `layer`、`passthrough`、`regions`、`combined` 的 `supported` 与 `reason`，并报告 `floatingOnly`、`backend`。可用性不代表已完成实机验收。

| 平台 | 原生实现及条件 |
| --- | --- |
| Windows | Win32 窗口事件维护层级，在分发鼠标前调整 HWND／WebView2 命中策略。覆盖层需要 REAPER 主窗口作为所有者。 |
| macOS | AppKit 层级与会话鼠标事件 tap。非 normal 鼠标模式需要 REAPER 的辅助功能权限，不自动弹出授权请求。 |
| Linux | 原生 X11、X Shape 1.1、XInput 2.1、支持 `_NET_RESTACK_WINDOW` 的窗口管理器和 REAPER 的 GLib 事件循环。输入形状与 WebKitGTK 画面独立。上述策略不支持 XWayland／Wayland。输入形状视口的每个轴必须小于 32768 原生像素。 |

`INVALID_ARGUMENT` 表示参数错误。`LAYER_UNAVAILABLE`、`MOUSE_MODE_UNAVAILABLE` 表示原生环境不支持。`LAYER_FAILED`、`MOUSE_MODE_FAILED` 表示原生设置或更新失败。运行时更新失败保留此前请求状态。异步初始化失败沿用现有窗口清理和诊断机制。

[单文件覆盖层示例](../examples/windows/overlay/README.zh-CN.md)默认使用英文，可切换简体中文。窗口交互模式复用无边框窗口能力，支持背景拖动和原生边缘缩放，输入控件排除在拖动区域之外。覆盖层仍位于 REAPER 浮动窗口下方。

在 macOS 使用鼠标路由前，打开**系统设置 → 隐私与安全性 → 辅助功能**，开启 **REAPER**。列表中没有该应用时，点击 **＋** 添加实际使用的 REAPER 应用，授权后退出并重新打开 REAPER。

Linux 无边框覆盖层允许超出显示器边界。示例向屏幕外多铺 1 个原生像素，避免窗口管理器将尺寸恰好等于整屏的无边框窗口提升到全屏层。App 铺满显示器时应保留此处理，或使用较小视口。恰好等于整屏的边界不具备跨 X11 窗口管理器的层级保证。

## SVG 控件

示例在同一个覆盖层内组合全部策略。`index.html` 内的 `mouseRegions()` 通过 `SVGGeometryElement.isPointInFill/isPointInStroke` 采样填充和描边，再合并为原生矩形区域。支持孔洞和变换，精度为 1 CSS 像素。普通 DOM 控件使用包围矩形。宿主仍接收矩形，不直接接收选择器或 SVG 路径。几何或布局改变后，App 需重新提交区域。裁剪、遮罩、非缩放描边和动画需要 App 自行维护区域。示例的鼠标手势使用 `requestPointerLock()`，拖动时隐藏光标，松开后恢复到起始位置。触摸和笔输入使用 `setPointerCapture`。macOS 和 Linux 宿主仅对独立覆盖层窗口授权 Pointer Lock，锁定被拒绝时终止鼠标调参并显示错误。

## Main 快捷键路由

```js
await reaper.window.setShortcutRouting(true);
const enabled = (await reaper.window.getState()).shortcutRouting;
// Lua: reaper.ReaWeb_SetShortcutRouting(id, true)
```

此策略显式启用后，原生键盘焦点仍留在 WebView。非编辑区域的可信字母、数字、标点、导航键、功能键和数字小键盘事件，按用户绑定转交 REAPER **Main** 动作区，包含 Shift、Ctrl、Alt 和 macOS Command 组合。不会固定执行播放动作，也不会改投 MIDI 编辑器动作区。按键归属及初始修饰键在重复和释放期间保持一致。失焦、关闭策略、导航或关闭窗口会释放已转交的按键。

文本框、textarea、contenteditable、可编辑 Shadow DOM、焦点 iframe 和 IME 组合输入留在页面内。点击文本框输入，点击非编辑控件后恢复 Main 快捷键。非编辑区域内未绑定的已路由按键会被消费。系统保留组合、在 DOM 之前被平台或浏览器处理的快捷键（包括开发者工具快捷键）、媒体键和任意国际键盘布局不在保证范围内。

默认关闭，重载后复位。`setShortcutRouting(false)` 成功时返回 `false`。启用时，对符合条件的按键优先于 `setKeyboardCapture` 和原有仅空格路由的 `setDawKeyboard`，不修改它们的设置。内嵌 JSFX WebUI 调用新接口返回 `WINDOW_HOSTED`。Lua 返回实际启用状态，错误通过 `ReaWeb_GetLastError()` 获取。能力信息见 `capabilities.runtime.window.shortcutRouting`。鼠标完全穿透不必转移键盘焦点，非编辑输入仍可按此策略路由。
