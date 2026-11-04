# Realtime 数据源

`reaper.realtime` 提供高频最新状态订阅。v0.3.9.23 在 Windows、macOS 和 Linux 支持 `transport`。请求频率不代表硬实时保证。

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

每个 Runtime 最多 32 个订阅，共享管理器和调度器。同一数据源在一次调度中只读取一次，各订阅按各自请求频率发布。最后一个订阅关闭后停止调度。页面关闭、导航和连接失败会释放对应资源。

`getCapabilities()` 查询数据源、格式和频率范围，不启动采样。初始化失败返回 `REALTIME_UNAVAILABLE`，未知数据源返回 `REALTIME_SOURCE_NOT_FOUND`。`getDiagnostics()` 返回当前窗口的订阅，以及共享的采样计数、错误和调度统计。

快照包含 `source`、`position`、`playState`、`sampledAt`、`sampleDurationMs`、`sequence`、`sourceSequence`、`projectEpoch`、`projectChanged` 和 `producerDropped`。完整字段和二进制布局见 [英文契约](realtime.md)。序号和 epoch 使用 bigint。

`position` 保留 `GetPlayPosition()` 的延迟补偿位置。`playState` 使用位掩码，1 为播放，2 为暂停，4 为录音。停止时不替换为编辑光标。数据源跟随活动工程，主线程观察到工程身份或加载代次变化时更新 epoch。该标记可能晚于实际切换，不是快照所属工程的原子标识。采样前后状态不一致时丢弃该次结果，多个 API 查询不构成宿主原子事务。

三平台均使用独立原生线程采样并发布。Windows 使用等待定时器，macOS 和 Linux 按单调时钟期限等待。后台线程仅调用 [Cockos Host Functions](https://www.cockos.com/reaper/sdk/vst/vst_ext.php#host_functions) 明确允许跨线程调用的 `GetPlayPosition()` 和 `GetPlayState()`。工程标记刷新和订阅管理保留在主线程。调度不依赖 Lua defer、Runtime tick 或原有 frame 定时器。错过的周期直接跳过，系统调度和浏览器负载仍可能延迟交付。

每秒查询或收到 120 个包，不代表每秒获得 120 个不同位置。位置精度取决于宿主和音频设备配置。实现不进行位置插值。暂停、停止或关闭音频设备后继续查询。`sampledAt` 是宿主单调时钟，不能直接与网页 `performance.now()` 相减。

## 兼容与扩展

后续数据源复用订阅、共享采样、调度和清理管理，分别定义格式、主线程上下文刷新及线程安全的采样逻辑。新增数据源必须先确认宿主 API 的线程约束。

Transport 使用独立的普通二进制 Native Stream，按最新状态合并。现有 Stream ABI、包头、JSFX `packet.timing`、实例生命周期和 frame 事件契约保持兼容。该接口不提供完整的状态变更事件历史。

[Demo](../examples/integrations/native-transport/README.md) 默认英语，支持中英文切换，提供 60/120 Hz 和 frame 对照、原生采样与页面接收统计及 CSV 导出。
