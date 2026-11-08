# Realtime Transport / 实时播放状态

## English

Requires ReaWebAPI v0.3.9.23 or later. Load and run `reawebapi-native-transport.lua` from REAPER's Action List. Lua opens the window and exits without a defer loop. The interface defaults to English. Use **Language** to switch to Simplified Chinese without restarting the subscription.

Select Realtime 60 Hz, Realtime 120 Hz or Frame comparison. Position and transport state update on each callback. Statistics and the chart refresh every 100 ms. The monitor shows native sampling, page delivery intervals and actual position changes separately. CSV export contains the latest 20,000 transport samples.

```js
await reaper.lifecycle.ready;
const subscription = await reaper.realtime.subscribe('transport', {
  updateRate: 120,
  onData(state) { console.log(state.position, state.playState); }
});
await subscription.close();
```

`position` is the unmodified `GetPlayPosition()` result. `playState` uses bits 1 playing, 2 paused and 4 recording. There is no interpolation. A 120 Hz packet rate does not imply 120 distinct positions per second. Host timestamps and page clocks cannot be subtracted directly. Sampling uses an independent native thread, while scheduling and browser load can delay delivery. Playback controls affect the current project. No JSFX is required.

Click **Read diagnostics** to view sampling, errors and subscription counters. See [Realtime sources](../../../docs/realtime.md).

With v0.3.9.29, the latest snapshot also shows `playRate`, position-specific `tempo` and `timeSignature`, and cumulative fractional `beatPosition` (QN). CSV includes these fields.

## 中文

需要 ReaWebAPI v0.3.9.23 或更高版本。在 REAPER Action List 中加载并运行 `reawebapi-native-transport.lua`。Lua 打开窗口后退出，没有 defer 循环。界面默认英语，通过 **Language** 切换为简体中文，不会重建订阅。

选择 Realtime 60 Hz、Realtime 120 Hz 或 Frame 对照。播放位置和走带状态随数据回调更新，统计及图表每 100 ms 刷新。面板分别显示原生采样频率、页面交付间隔及位置实际变化频率。CSV 保存最近 20,000 个播放状态样本。订阅代码见上方示例。

`position` 保留 `GetPlayPosition()` 原始结果。`playState` 位掩码为 1 播放、2 暂停、4 录音。不进行位置插值，每秒收到 120 个包不代表获得 120 个不同位置。宿主时间戳不能与网页时钟直接相减。采样使用独立原生线程，系统调度和浏览器负载仍可能延迟交付。播放按钮控制当前工程，无需 JSFX。

点击 **读取原生诊断** 可查看采样、错误及订阅计数。接口约定见 [Realtime 数据源](../../../docs/realtime.zh-CN.md)。

v0.3.9.29 的最新快照及 CSV 包含 `playRate`、当前位置的 `tempo` 和 `timeSignature`，以及支持小数的累计 `beatPosition`（QN）。
