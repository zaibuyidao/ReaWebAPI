# DevTools

[English](devtools.md) | **简体中文**

Windows/Linux 下，在 WebView 或其受管 DevTools 窗口中按 **Ctrl+Shift+I** 可显示或隐藏 DevTools。长按不会重复切换，隐藏后焦点返回页面。`reaper.debug.openDevTools()` 和 `ReaWeb_DevTools(id)` 请求打开或显示 DevTools，重复调用不会隐藏检查器。

| 平台 | 显示方式 | 模式切换 |
| --- | --- | --- |
| Linux / WebKitGTK | 默认嵌入右侧，提供可拖动分隔条 | 面板工具栏的 **Float DevTools** / **Dock right** |
| Windows / WebView2 | 默认嵌入右侧，提供可拖动分隔条 | 页面右键菜单的 **Float DevTools** / **Embed DevTools** |
| macOS / WKWebView | Safari Web Inspector | Ctrl+Shift+I 切换操作指引，实际检查器由 Safari 控制 |

## Linux

面板默认占可用宽度的 40%，拖动分隔条可调整至 20%–80%。调整主窗口大小时保留比例。**Float DevTools** / **Dock right** 在容器间移动同一个检查器视图，保留 Console / Inspector 会话，不重载页面。

Ctrl+Shift+I 在检查器内同样有效。快捷键、工具栏 **Hide DevTools** 和浮动窗口关闭按钮只隐藏面板并保留会话。检查器自带的关闭控件可能结束会话，再次打开时检查器状态可能重置。

浮动窗口为非模态窗口，通过 X11 关联当前 REAPER 父窗口，层级受窗口管理器控制。需要 X11 或 XWayland。

## Windows

**Embedded** 将 WebView2 原生检查器嵌入当前 ReaWebAPI 窗口右侧。面板默认占可用宽度的 40%，拖动分隔条可调整至 20%–80%，调整窗口大小时保留比例。**Floating** 使用独立宿主窗口。两种模式复用同一个检查器窗口，保留 Console / Inspector 状态，不重载页面。

在 WebView 页面上右键，DevTools 操作与 **Dock in REAPER** / **Undock from REAPER** 位于同一菜单。隐藏时显示 **Open DevTools**，显示时提供 **Hide DevTools**。嵌入模式提供 **Float DevTools**，浮动模式提供 **Embed DevTools**。隐藏时切换模式只保存偏好，不打开检查器。检查器填满容器，不附加宿主工具栏，Demo 也不再单独提供 DevTools 按钮。

右键菜单、快捷键和 API 共用状态管理。Ctrl+Shift+I、**Hide DevTools** 和浮动宿主窗口关闭按钮只隐藏检查器并保留会话，检查器自身的关闭操作可能结束会话。浮动窗口跟随当前 REAPER 根窗口，其他 REAPER 窗口激活时不抢焦点地提升层级，不设置全局置顶。焦点移至 DevTools 不会暂停或重载页面。

WebView2 没有公开的嵌入式检查器控制器。ReaWebAPI 通过 [`OpenDevToolsWindow`](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2#opendevtoolswindow) 创建检查器，识别原生窗口后由 Win32 容器托管。Per-Monitor V1/V2 宿主通过兼容的 DPI 容器托管，不修改 REAPER 或 Chromium 的进程 DPI 模式。不支持的 DPI 组合或托管失败时保留原生浮动窗口，并禁用 **Embed DevTools**，`devtools.fallbackReason` 返回降级原因。窗口识别或检查器内按键处理失败时，`devtools.lastError` 返回对应限制。无法识别的窗口需使用自身关闭按钮。此集成依赖 WebView2 原生窗口实现。

## macOS

ReaWebAPI 允许通过 Safari Web Inspector 检查 WKWebView 页面：

1. 在 **Safari Settings > Advanced** 开启 **Show features for web developers**。
2. 选择 **Develop > this Mac > REAPER > 对应页面**。

在页面或指引中按 Ctrl+Shift+I 均会切换同一个非模态指引窗口，隐藏后焦点返回页面。`reaper.debug.openDevTools()` 显示指引，并以 `INSPECTOR_MENU` 错误拒绝 Promise。实际检查器由 Safari 打开和关闭，ReaWebAPI 不管理其布局或窗口层级。

## 状态保存与诊断

`ReaWebAPI/WindowState/*.json` 按页面及窗口槽位保存 `devtools.mode`（`embedded` / `floating`）和 `devtools.widthRatio`。重新打开工具或重启 REAPER 后恢复偏好。DevTools 初始关闭，不持久化调试会话。Windows/Linux 恢复已保存模式，包括旧版本保存的浮动偏好。Windows 托管降级不会覆盖已保存偏好。macOS 使用浮动模式并保留宽度偏好。

`(await reaper.debug.getDiagnostics()).devtools` 返回实际模式、宽度比例、嵌入支持情况及后端状态。Safari 检查器不提供可见状态，Windows 的可见状态仅反映 ReaWebAPI 已识别的原生窗口。验证步骤见[手动检查清单](SMOKE_TEST.md#devtools-acceptance)。
