# Realtime 数据源

`reaper.realtime` 在 Windows、macOS 和 Linux 提供 `transport`、`track-controls` 和 `fx-parameters` 数据源。请求频率最高 120 Hz，实际频率取决于数据源和宿主调度。

```js
await reaper.lifecycle.ready;
const caps = await reaper.realtime.getCapabilities();
if (!caps.sources.transport.available) throw new Error('Transport unavailable');
const subscription = await reaper.realtime.subscribe('transport', {
  updateRate: 120,
  onData(state) { render(state.position, state.playState); },
  onError(error) { console.error(error); }
});
await subscription.close();
```

`updateRate` 范围为 1–120 Hz，默认 60。`onData`、`onError`、`onClose` 可选。返回对象提供 `source`、`updateRate`、`latest()`、`closed`、`error` 和幂等异步 `close()`。`latest()` 返回最近快照，无数据或已关闭时返回 null。

每个 Runtime 最多 32 个订阅，共享一个管理器。播放状态订阅共用原生采样线程，最后一个播放状态订阅关闭后停止该线程。轨道控制使用主线程采集，仅发布变化。页面关闭、导航和连接失败会释放对应资源。

`getCapabilities()` 查询数据源、格式和频率范围，不启动采样。初始化失败返回 `REALTIME_UNAVAILABLE`，未知数据源返回 `REALTIME_SOURCE_NOT_FOUND`。`getDiagnostics()` 返回当前窗口的订阅，以及共享的采样计数、错误和调度统计。

播放状态快照包含 `source`、`position`、`playState`、`playRate`、`tempo`、`timeSignature`、`beatPosition`、`sampledAt`、`sampleDurationMs`、`sequence`、`sourceSequence`、`projectEpoch`、`projectChanged` 和 `producerDropped`。完整字段和二进制布局见 [英文契约](realtime.md)。序号和 epoch 使用 bigint。

`position` 保留 `GetPlayPosition()` 的延迟补偿位置。`playState` 使用位掩码，1 为播放，2 为暂停，4 为录音。停止时不替换为编辑光标。数据源跟随活动工程，主线程观察到工程身份或加载代次变化时更新 epoch。该标记可能晚于实际切换，不是快照所属工程的原子标识。采样前后状态不一致时丢弃该次结果，多个 API 查询不构成宿主原子事务。

三平台均使用独立原生线程采样并发布。Windows 使用等待定时器，macOS 和 Linux 按单调时钟期限等待。音乐数据通过 `Master_GetPlayRateAtTime`、`TimeMap_GetTimeSigAtTime` 和 `TimeMap_timeToQN_abs` 按同帧 position 查询，由 REAPER 处理渐变 BPM 和拍号变化。工程标记刷新和订阅管理保留在主线程。调度不依赖 Lua defer、Runtime tick 或原有 frame 定时器。错过的周期直接跳过，系统调度和浏览器负载仍可能延迟交付。

每秒查询或收到 120 个包，不代表每秒获得 120 个不同位置。位置精度取决于宿主和音频设备配置。实现不进行位置插值。暂停、停止或关闭音频设备后继续查询。`sampledAt` 是宿主单调时钟，不能直接与网页 `performance.now()` 相减。

## 兼容与扩展

后续数据源复用订阅、共享采样、调度和清理管理，分别定义格式、主线程上下文刷新及线程安全的采样逻辑。新增数据源必须先确认宿主 API 的线程约束。

Transport 使用独立的普通二进制 Native Stream，按最新状态合并。现有 Stream ABI、包头、JSFX `packet.timing`、实例生命周期和 frame 事件契约保持兼容。该接口不提供完整的状态变更事件历史。

[Demo](../examples/integrations/native-transport/README.md) 默认英语，支持中英文切换，提供 60/120 Hz 和 frame 对照、原生采样与页面接收统计及 CSV 导出。

`playRate` 是播放速率倍数。`tempo` 是 position 对应的 Tempo Map BPM，不乘播放速率。`timeSignature` 包含 `numerator` 和 `denominator`。`beatPosition` 是工程累计 QN，保留小数，不是小节内拍数。

新版使用 96 字节 `transport.v2`，运行时保留旧版解码。新增字段需要 v0.3.9.29。循环开关和边界继续使用现有 API，播放、暂停和录音状态已由 `playState` 提供。

## 轨道控制

使用 `reaper.audio.getSelectedTrackRef()` 获取普通轨道的 `guid`。向 `subscribe("track-controls", { tracks: [guid], properties: ["volume", "solo"], onData })` 传入 1–16 个互不重复的 GUID。首包返回当前快照，后续仅发布变化。返回轨道顺序与请求一致，省略 `properties` 会返回全部四项。

音量为线性增益（1 = 0 dB），声像使用原始 `D_PAN`，静音为布尔值，Solo 保留 `I_SOLO` 原生整数。控制面通知触发主线程回读，周期快照补充脚本写入和遗漏的通知。无通知时回读最高 30 Hz。多个窗口共享同一轮轨道属性读取，最后一个订阅结束后停止采集。

轨道重排保留订阅，轨道删除或工程切换会终止整组订阅，通过 `onError` 和 `onClose` 返回 `STALE_HANDLE` 或 `PROJECT_CHANGED`。诊断提供实际平均采集与发布频率、采样耗时、采样至原生发布的耗时、触发来源、跳过周期和发布失败次数。页面到达间隔使用本地时钟测量。协议布局和示例见[英文文档](realtime.md#track-controls)。

## FX 参数

`subscribe('fx-parameters', {track, fx, parameters, updateRate})` 默认观察普通轨道或 Master 的主链（`chain: "main"`）。输入链用 `"input"`，监听链用 `"monitoring"` 和 Master GUID，Take FX 用 `"take"` 并传入所属 `take` GUID。各链上的嵌套 Container 子 FX 都按自身 GUID 定位。通过 ReaScript 的 `GetSetMediaItemTakeInfo_String` 读取 Take GUID，`TakeFX_GetFXGUID` 读取 Take FX GUID。通过 ReaScript 的 `GetTrackGUID` 和 `TrackFX_GetFXGUID` 获取轨道与 FX GUID。`parameters` 接受 1–32 个互不重复的零基参数索引。

首次推送快照，之后仅推送变化。`values` 按请求顺序返回 `{index, normalized}`，数值来自 `TrackFX_GetParamNormalized` 或 `TakeFX_GetParamNormalized`（0–1）。主线程根据宿主通知和定期快照采样，同一轮共享重复读取。请求上限为 1–120 Hz，默认 60 Hz。自动化可能跳过中间值，只能观察宿主公开的参数变化。

重排后继续跟踪原 FX。目标删除、移出主链或参数消失时返回 `STALE_HANDLE`，切换工程返回 `PROJECT_CHANGED`。订阅时参数越界或重复返回 `INVALID_ARGUMENT`。页面诊断的 `received`、`receiveHz` 和 `sequenceGaps` 分别记录接收数、接收频率和序号缺口，可与宿主采样、发布及丢弃计数对照。
