# Lua backend + WebView UI

Run `Open.lua` in REAPER, select a track, then adjust the volume slider. Lua owns REAPER access and polls `ReaWeb_Receive` with `defer`. The page uses only `reaper.host.send` and the `message` event. Closing the window ends the backend. Stopping the script closes its window.

Lua sends JSON state with escaped track names. The UI sends `ready` and `volume <linear-amplitude>` text commands. The ready message requests a fresh snapshot after each reload. Applications can instead send JSON values with `host.send` and decode the resulting text with their Lua JSON library.

在 REAPER 中运行 `Open.lua`，选择轨道后调整音量滑块。Lua 使用普通 REAPER API，并通过 `defer` 轮询消息。WebView 仅负责 UI。关闭窗口会结束后端，停止脚本会关闭窗口。页面重载后通过 `ready` 请求最新状态。
