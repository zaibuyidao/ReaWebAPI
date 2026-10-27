# ReaWebAPI

- 修复 `ReaWeb_Open` 可选布尔参数绑定，支持 Lua `false/true` 和 EEL2 `0/1`，保持 Windows、macOS、Linux 上省略参数及窗口复用行为不变。
- 新增 EEL2 WebView 请求响应示例，以及音量、声像、颜色、Mute、Solo 双向轨道控制示例，支持帧通道刷新、`0..4` 音量范围、推子执行确认、未确认请求限流、最终值保护和关闭时清理。
- SDK 收录 EEL2 示例和双语支持文档，ReaPack 注册示例启动脚本。
