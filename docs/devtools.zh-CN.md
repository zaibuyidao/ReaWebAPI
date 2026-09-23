# DevTools

[English](devtools.md) | **简体中文**

在 WebView 或其受管 DevTools 窗口中，Windows/Linux 按 **Ctrl+Shift+I**，macOS 按 **Option+Command+I** 显示或隐藏 DevTools。长按不会重复切换，隐藏后焦点返回页面。`reaper.debug.openDevTools()` 和 `ReaWeb_DevTools(id)` 请求打开或显示 DevTools，重复调用不会隐藏检查器。

| 平台 | 显示方式 | 模式切换 |
| --- | --- | --- |
| Linux / WebKitGTK | 默认嵌入右侧，提供可拖动分隔条 | 面板工具栏的 **Float DevTools** / **Dock right** |
| Windows / WebView2 | 默认嵌入右侧，提供可拖动分隔条 | 页面右键菜单的 **Float DevTools** / **Embed DevTools** |
| macOS / WKWebView | 默认嵌入右侧，提供可拖动分隔条 | 页面右键菜单的 **Float DevTools** / **Embed DevTools** |

## Linux

面板默认占可用宽度的 40%，拖动分隔条可调整至 20%–80%。调整主窗口大小时保留比例。**Float DevTools** / **Dock right** 在容器间移动同一个检查器视图，保留 Console / Inspector 会话，不重载页面。

Ctrl+Shift+I 在检查器内同样有效。快捷键、工具栏 **Hide DevTools** 和浮动窗口关闭按钮只隐藏面板并保留会话。检查器自带的关闭控件可能结束会话，再次打开时检查器状态可能重置。

浮动窗口为非模态窗口，通过 X11 关联当前 REAPER 父窗口，层级受窗口管理器控制。需要 X11 或 XWayland。

## Windows

**Embedded** 以 WebView2 检查器内容填满右侧面板，隐藏标题栏、窗口控制按钮及边框，不保留装饰占位或可拖动标题栏。面板默认占可用宽度的 40%，拖动 1px 分割线可调整至 20%–80%，调整窗口大小时保留比例。分割线使用系统窗口边框颜色。**Floating** 恢复检查器原生标题栏、窗口控制按钮和边框，不再叠加宿主窗口。模式切换复用同一个检查器窗口，保留 Console / Inspector 状态，不重载页面。

在 WebView 页面上右键，DevTools 操作与 **Dock in REAPER** / **Undock from REAPER** 位于同一菜单。隐藏时显示 **Open DevTools**，显示时提供 **Hide DevTools**。嵌入模式提供 **Float DevTools**，浮动模式提供 **Embed DevTools**。隐藏时切换模式只保存偏好，不打开检查器。检查器填满容器，不附加宿主工具栏，Demo 也不再单独提供 DevTools 按钮。

右键菜单、快捷键和 API 共用状态管理。Ctrl+Shift+I 和 **Hide DevTools** 只隐藏检查器并保留会话。浮动窗口的原生关闭按钮会结束检查器会话。浮动窗口跟随当前 REAPER 根窗口，其他 REAPER 窗口激活时不抢焦点地提升层级，不设置全局置顶。焦点移至 DevTools 不会暂停或重载页面。Windows 嵌入式 DevTools 支持多个停靠 WebView 之间的切换，包括 REAPER 与 WebView2 使用不同 DPI 上下文的情况。

WebView2 没有公开的嵌入式检查器控制器。ReaWebAPI 通过 [`OpenDevToolsWindow`](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2#opendevtoolswindow) 创建并识别原生检查器窗口，在 Embedded 模式下依据实际渲染起点和客户区边界裁去 Chromium 自绘标题栏及边框。Per-Monitor V1/V2 宿主通过兼容的 DPI 容器托管，不修改 REAPER 或 Chromium 的进程 DPI 模式。内容边界不可用、不支持的 DPI 组合或托管失败时保留原生浮动窗口，并禁用 **Embed DevTools**，`devtools.fallbackReason` 返回降级原因。窗口识别或检查器内按键处理失败时，`devtools.lastError` 返回对应限制。无法识别的窗口需使用自身关闭按钮。此集成依赖 WebView2 原生窗口实现。

## macOS

**Embedded** 将原生检查器内容视图放在页面右侧，不显示独立窗口标题栏、窗口按钮、外层边框，也不预留装饰空间。**Floating** 将同一视图放回 WebKit 自身的检查器窗口，不额外创建 ReaWebAPI 窗口或工具栏。

页面右键菜单根据当前状态显示 **Open DevTools** / **Hide DevTools** 和 **Float DevTools** / **Embed DevTools**。在页面或检查器中按 **Option+Command+I** 切换显示，隐藏时切换模式不会打开检查器。快捷键、菜单、`reaper.debug.openDevTools()` 和 `ReaWeb_DevTools(id)` 共用控制器。隐藏和切换模式保留检查器视图及连接，包括 Console 日志和当前选中的标签。原生关闭控件会结束会话。

面板默认占可用宽度的 40%，拖动检查器分隔线可调整至 20%–80% 并保存比例。调整窗口大小时保留比例，窄窗口同样支持嵌入，不会因宿主尺寸禁用 **Embed DevTools** 或强制切换为 Floating。

公开的 `WKWebView.inspectable` 保持启用。公开 API 没有对应控制器，程序化控制与显示使用经过运行时能力检查的 WebKit 私有接口。AppKit 负责内容视图布局，检查器的停靠控件复用现有模式和宽度偏好。兼容性取决于系统 WebKit 版本，嵌入控制不可用时保留原生浮动窗口，`embeddedSupported` 返回 `false`。原生控制不可用时禁用菜单操作，`nativeToggleSupported` 返回 `false`，`openDevTools()` 以 `DEVTOOLS_UNAVAILABLE` 错误拒绝。仍可通过 Safari 的开发菜单手动检查页面。`fallbackReason` 和 `lastError` 提供能力限制或打开失败的原因。

## 状态保存与诊断

`ReaWebAPI/WindowState/*.json` 按页面及窗口槽位保存 `devtools.mode`（`embedded` / `floating`）和 `devtools.widthRatio`。重新打开工具或重启 REAPER 后恢复偏好。DevTools 初始关闭，不持久化调试会话。各平台恢复已保存模式，包括旧版本保存的浮动偏好。Windows/macOS 托管降级不会覆盖已保存偏好。

`(await reaper.debug.getDiagnostics()).devtools` 返回实际模式、宽度比例、嵌入支持情况及后端状态。macOS 的 `visible` 反映本地 WebKit 检查器状态，Windows 的可见状态仅反映 ReaWebAPI 已识别的原生窗口。验证步骤见[手动检查清单](SMOKE_TEST.md#devtools-acceptance)。
