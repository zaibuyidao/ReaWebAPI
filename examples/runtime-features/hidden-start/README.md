# Hidden startup / 隐藏启动

Run `reawebapi-hidden-start.lua` with ReaWebAPI v0.3.8.15 or later. The App initializes while its native window remains hidden. Lua shows it without forcing focus after receiving the App-defined `ui-ready` message. A 30-second timeout closes the window if initialization fails. `ReaWeb_IsReady` alone does not indicate business UI readiness.

On Windows, use v0.3.8.27 or later for framed hidden windows to render correctly on the first `Show` or `Focus`.

使用 ReaWebAPI v0.3.8.15 或更高版本运行 `reawebapi-hidden-start.lua`。App 在原生窗口隐藏期间初始化，Lua 收到自定义 `ui-ready` 消息后显示窗口，不强制聚焦。30 秒内未完成初始化则关闭窗口。`ReaWeb_IsReady` 本身不表示业务界面已就绪。

Windows 有边框隐藏窗口请使用 v0.3.8.27 或更高版本，确保首次 `Show` 或 `Focus` 正常绘制页面。

For JavaScript child windows, check `capabilities.runtime.window?.hiddenStart` and use `await reaper.window.open(path, {hidden:true})`. The child can call `await reaper.window.show()` when its own UI is ready.

JavaScript 子窗口先检查 `capabilities.runtime.window?.hiddenStart`，再使用 `await reaper.window.open(path, {hidden:true})`。子页面可在业务界面就绪后调用 `await reaper.window.show()`。
