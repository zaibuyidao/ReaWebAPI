# DevTools

[English](devtools.md) | **简体中文**

Windows/Linux 下，在 WebView 或其受管 DevTools 窗口中按 **Ctrl+Shift+I** 可显示或隐藏 DevTools。长按不会重复切换，隐藏后焦点返回页面。`reaper.debug.openDevTools()` 和 `ReaWeb_DevTools(id)` 请求打开或显示 DevTools，重复调用不会隐藏检查器。

| 平台 | 显示方式 | 模式切换 |
| --- | --- | --- |
| Linux / WebKitGTK | 默认嵌入右侧，提供可拖动分隔条 | 面板工具栏的 **Float DevTools** / **Dock right** |
| Windows / WebView2 | 原生浮动窗口 | 当前实现仅支持浮动模式 |
| macOS / WKWebView | Safari Web Inspector | Ctrl+Shift+I 切换操作指引，实际检查器由 Safari 控制 |

## Linux

面板默认占可用宽度的 40%，拖动分隔条可调整至 20%–80%。调整主窗口大小时保留比例。**Float DevTools** / **Dock right** 在容器间移动同一个检查器视图，保留 Console / Inspector 会话，不重载页面。

Ctrl+Shift+I 在检查器内同样有效。快捷键、工具栏 **Hide DevTools** 和浮动窗口关闭按钮只隐藏面板并保留会话。检查器自带的关闭控件可能结束会话，再次打开时检查器状态可能重置。

浮动窗口为非模态窗口，通过 X11 关联当前 REAPER 父窗口，层级受窗口管理器控制。需要 X11 或 XWayland。

## Windows

Windows 后端使用 WebView2 的 [`OpenDevToolsWindow`](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2#opendevtoolswindow)。当前未实现嵌入模式和嵌入/浮动切换。

ReaWebAPI 识别到原生窗口后，快捷键在 DevTools 获得焦点时同样有效，隐藏或显示同一个窗口并保留会话。原生关闭按钮会结束会话。无法识别窗口时，请使用窗口自己的关闭按钮。通过原生右键菜单 **Inspect** 打开的检查器可能出现此情况。如果 DevTools 内的按键处理不可用，请在主 WebView 中使用快捷键。诊断字段 `devtools.lastError` 会说明这些限制。

识别到的窗口为非模态窗口，其所有者设为当前 REAPER 根窗口，不设置全局置顶。焦点移至 DevTools 不会暂停或重载主 WebView。

## macOS

ReaWebAPI 允许通过 Safari Web Inspector 检查 WKWebView 页面：

1. 在 **Safari Settings > Advanced** 开启 **Show features for web developers**。
2. 选择 **Develop > this Mac > REAPER > 对应页面**。

在页面或指引中按 Ctrl+Shift+I 均会切换同一个非模态指引窗口，隐藏后焦点返回页面。`reaper.debug.openDevTools()` 显示指引，并以 `INSPECTOR_MENU` 错误拒绝 Promise。实际检查器由 Safari 打开和关闭，ReaWebAPI 不管理其布局或窗口层级。

## 状态保存与诊断

`ReaWebAPI/WindowState/*.json` 按页面及窗口槽位保存 `devtools.mode`（`embedded` / `floating`）和 `devtools.widthRatio`。重新打开工具或重启 REAPER 后恢复偏好。DevTools 初始关闭，不持久化调试会话。Windows/macOS 恢复为浮动模式并保留宽度偏好。

`(await reaper.debug.getDiagnostics()).devtools` 返回实际模式、宽度比例、嵌入支持情况及后端状态。Safari 检查器不提供可见状态，Windows 的可见状态仅反映 ReaWebAPI 已识别的原生窗口。验证步骤见[手动检查清单](SMOKE_TEST.md#devtools-acceptance)。
