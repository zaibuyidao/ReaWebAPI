# Realtime Transport Monitor

Run `reawebapi-native-transport.lua` from REAPER’s Action List with ReaWebAPI installed. The launcher opens `index.html` and exits. The page owns the subscription and controls playback in the current project.

- **Sampling.** Compare 60 Hz and 120 Hz transport subscriptions with REAPER frame events. Position comes directly from `GetPlayPosition()`. Several samples can contain the same position.
- **Timing.** Native sample intervals use the host clock. Arrival intervals use the page clock. CSV export retains the latest 20,000 samples, including sequence gaps and transport state.
- **Track and FX controls.** Track subscriptions follow the track GUID across reordering. FX parameters use a zero-based index and normalized values in the range 0–1.

[Realtime API reference](../../../docs/realtime.md)

## 实时播放状态

安装 ReaWebAPI 后，在 REAPER 的动作列表中运行 `reawebapi-native-transport.lua`。启动器打开 `index.html` 后退出，由页面管理订阅并控制当前工程的播放。

- **采样。** 可将 60 Hz、120 Hz 的播放状态订阅与 REAPER 的帧事件对照。播放位置直接取自 `GetPlayPosition()`，连续多个样本可以具有相同的位置值。
- **计时。** 原生采样间隔使用宿主时钟，到达间隔使用页面时钟。CSV 可导出最近 20,000 个样本，包含序号缺口和播放状态。
- **轨道与 FX 控制。** 轨道订阅按 GUID 跟踪，轨道重排后仍指向同一轨道。FX 参数索引从 0 开始，参数值使用 0–1 的归一化范围。

[实时数据源 API 参考](../../../docs/realtime.zh-CN.md)
