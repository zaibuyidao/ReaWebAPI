# Runtime Diagnostics

[English](diagnostics.md) | **简体中文**

Windows、macOS、Linux 上的普通 App 与 JSFX WebUI 共用持久化诊断日志。在 **Preferences > Plug-ins > ReaWebAPI > Diagnostics** 配置。

| 设置 | 作用 |
| --- | --- |
| Enable App debug logging + Target App ID | 为精确匹配的 App ID 及其关联 Runtime 事件启用 INFO/DEBUG，覆盖该 App 的全部实例 |
| Enable Runtime debug logging | 为 Runtime 底层事件启用 INFO/DEBUG，包括尚无法归属 App 的事件，不开启其他 App 的 JavaScript debug/info 记录 |
| Open diagnostics log folder | 在系统文件管理器中打开日志目录 |

两个开关默认关闭，互不依赖。Apply/OK 将设置保存到 `ReaWebAPI.ini`，对已运行实例即时生效。可在 App 启动前填写目标，在现有 App ID 可识别后记录初始化过程，无需修改 App 配置。目标采用现有 `reaper.app.getId()` 或 `webRuntime.appId`，包含自动生成的 JSFX ID。空目标不能启用 App DEBUG。

日志位于 `<REAPER 资源目录>/ReaWebAPI/Diagnostics/`，格式为 UTF-8 JSON Lines，可直接用文本编辑器阅读。`runtime.log` 保存 Runtime 事件，`app-<appId>.log` 保存该 App 及其关联 Runtime 事件。单文件上限 2 MiB，保留三份编号备份。目录最多保留 64 个受管日志文件，达到上限时删除最旧文件。文件名保留现有小写 ASCII App ID，不截断，日志目标最长 240 个字符。诊断目标校验不改变 App 身份。

每条记录包含 UTC `time`、`level`（`ERROR`、`WARN`、`INFO`、`DEBUG`）、`scope`、`event`、诊断 `sessionId`、`appId`、`windowId`、`result` 和 `threadId`。诊断会话标识一次 Runtime 生命周期，与认证及文档令牌独立。同一 App 的多个实例通过窗口 ID 区分。可用的 JSFX 上下文包含 `fxGuid`、`fxIndex`、`fxLocation`、`fxSessionId` 和 `activation`。API 执行记录可包含 `method` 和 `durationMs`。早期事件的 App ID 为空，窗口 ID 为 0。

事件覆盖创建、Bridge 就绪与导航、关闭与释放、实例绑定、API 失败、JavaScript 日志等级、宿主消息通道活动及失败、生命周期超时，以及 JSFX 实例识别、暂存、关联、失效和原生通道会话变化。Track、输入 FX、Item/Take 和嵌套 Container FX 复用现有定位机制，FX 索引保留宿主的 Container 编码。

默认仅持久化 WARN/ERROR。自由文本、URL、调用参数、通信正文、认证令牌和音频样本不写入文件，包括 `reaper.debug.log/warn/error` 的文本。现有内存日志及控制台输出行为不变。相同 App、窗口和事件每秒最多记录 20 条，后续记录的 `suppressed` 表示被抑制数量。日志在 UI 线程写入并刷新，其他线程在 I/O 前直接返回。诊断 I/O 失败不影响 Runtime 运行。

REAPER 没有公开 JSFX 编译回调。原生通道会话变化、宿主 FX 修订通知、参数数量变化及绑定失效均为观测结果，不能据此断言发生了编译。Runtime 也无法直接观察任意 Lua 脚本启停。`lua.channel.begin/end` 表示现有宿主消息通道的可观测使用及释放，不代表 Lua 进程生命周期。Lua 轮询停止但窗口仍打开时，没有可靠的退出信号。已刷新的记录可在进程强制结束后读取，不保证断电持久性。

Demo 的 Diagnostics 面板及 `reaper.debug.getDiagnostics()` 继续提供状态快照。自动记录持久化日志无需保持任何查看窗口打开。

主动诊断仅在 Runtime 调试开启，或 App 调试开启且 Target App ID 匹配时运行。Runtime 调试覆盖全部实例，App 调试仅覆盖目标 App。这些设置控制探测、未完成请求跟踪、浏览器计数、宿主调度间隔及慢请求检查。关闭后停止对应监测并清空健康快照。已有被动 WARN/ERROR 记录继续保留，包括 API 失败和平台报告的 WebView 进程故障。

受监测且可见、就绪的普通 App 与 JSFX WebUI 共用每 5 秒一次的探测，同时最多保留一个未应答探测。隐藏窗口、停放中的 JSFX、导航和生命周期清理暂停探测。正常报告仅记录 DEBUG。启用监测或文档导航时重置浏览器计数。启用前已存在的请求从启用时开始计时，方法名可能为空。

| 事件 | 含义 |
| --- | --- |
| `bridge.unresponsive` / `bridge.responsive` | 探测超过 15 秒未应答 / 应答恢复。可能是页面事件循环或任一通信方向受阻，不能据此断言崩溃。 |
| `bridge.slow` | 探测往返耗时至少 1 秒。 |
| `bridge.probe.fail` | 诊断探测发送失败，不因此关闭 App。 |
| `bridge.request.stalled` | 页面仍能应答探测，但最早未完成请求已超过 30 秒。模态窗口或异步工作可能正常等待。仅记录一次，直至报告低于阈值或监测重置。 |
| `runtime.tick.delayed` | 宿主定时回调在至少 2 秒的间隔后恢复。模态窗口、系统休眠或宿主停顿均可能触发。此时丢弃未完成探测，避免将宿主停顿归因于页面。 |
| `api.slow` | 从原生端收到请求到准备回复至少 500 ms，包含排队及异步工作，不等同于执行耗时。 |
| `javascript.health.warning` | 浏览器超时、队列拒绝、发送或编码失败、脚本异常、Promise 拒绝、回调异常或意外流关闭的累计计数增加。 |
| `webview.process.fail` | 平台报告 WebView 进程故障。 |

慢探测、探测发送失败与慢请求警告共用每窗口 30 秒冷却时间。宿主调度停顿警告采用 Runtime 级别的 30 秒冷却时间。探测超时仅记录一次，直至收到应答或监测状态重置。检查复用 UI 定时回调，不增加看门狗线程，不在音频线程写日志。

页面报告包含 `pagePending`、`oldestAgeMs`、`oldestStarted`、最早未完成请求的 `method`、最近超时的 `timeoutMethod`、累计异常计数、`scriptLine`/`scriptColumn`、`activeStreams` 和 `streamPackets`。原生上下文包含 `pendingCalls`、`queuedCalls`、`outputPending`、`hostMessages` 和 `webMessages`。应结合传输状态比较连续的流计数，音频停止或生产者空闲时没有新数据可能是正常情况。已开始的请求可能正在等待原生对话框或异步操作。`api.fail` 包含方法名，服务完成记录包含 `service`/`operation`，不记录参数或流正文。快照中的 `bridgeHealth` 包含 `enabled`、最后一次有效页面报告与当前探测标志，不是即时采样。查询快照不会开启监测。

REAPER UI 线程永久阻塞时无法写入新日志或运行探测。受限流影响的 DEBUG API begin/end 记录可缩小最后一次原生调用的范围，不能替代崩溃堆栈。宿主停顿只能在 UI 恢复后观测。隐藏页面不检测存活状态，脚本坐标不含源 URL。日志不足以定位时仍需进程转储或调试器。

跨源隔离失败在桥接握手前向宿主报告。`webRuntime.isolation.fail` 记录请求策略、文档代次及受限的浏览器结果。能力探针不依赖调试日志开关，结果位于 JS 和原生诊断的 `webRuntime`。详见 [隔离诊断](frontend.zh-CN.md#app-级跨源隔离)。
