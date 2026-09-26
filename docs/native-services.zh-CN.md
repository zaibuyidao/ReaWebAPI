# Native Event 与 Host Service

`reaper.host.send(message)` 保留现有 Lua Backend 消息格式和路由。`reaper.host.service(name)` 访问内建或第三方原生服务。`reaper.events` 承载 Runtime 与 REAPER 状态事件。三者可同时使用。WebView 和原生通信由 REAPER 原生定时器维持，不依赖 Lua `defer`。

```js
const test = reaper.host.service('test');
const callback = payload => console.log(payload);
const dispose = await test.on('changed', callback);
const pong = await test.invoke('ping');
test.send('message', {text: 'hello'});
await test.off('changed', callback);
await dispose();
```

`service(name)` 创建本地代理，可以先于注册调用。`invoke(method, payload = null)` 返回 Promise。`send(method, payload = null)` 返回 `void`，分发失败写入 Runtime 错误日志。服务、方法和事件名称由 1–128 个 ASCII 字母、数字、`_`、`-`、`.` 组成。载荷和结果为 JSON，上限 1 MiB。

`on` 返回 `Promise<Dispose>`，与现有事件 API 一致。`off` 移除该事件中同一 callback 的全部注册。服务事件按顺序投递，不合并，不重放初始状态。`unloaded` 为保留事件。服务注销时，pending invoke 以 `EXTENSION_UNLOADED` 拒绝，并清除订阅。重新注册后需重新订阅。旧句柄不能访问同名新服务。关闭和重载会清理 pending invoke 与订阅。原生执行后 30 秒仍未完成的调用返回 `TIMEOUT`。取消不回滚已执行的业务操作。

内建 `runtime` 服务提供 `getInfo`，返回 `{version, serviceABI}`，与第三方服务使用同一注册机制。状态查询和控制继续复用 Mirror。

## C/C++ Extension

公共 ABI 见 [reaweb_service.h](../src/public/reaweb_service.h)，完整示例见 [test extension](../tests/native_service_extension.cpp)。通过 REAPER `GetFunc` 获取 `ReaWeb_RegisterService`、`ReaWeb_UnregisterService`、`ReaWeb_CompleteServiceCall`、`ReaWeb_EmitServiceEvent`。这些入口不属于 Mirror 或 Lua vararg API。

SDK 包将公共头文件与示例放在 `native/`。编译示例需要官方 REAPER Extension SDK，并将该目录加入头文件搜索路径。

注册时传入唯一服务名、包含大小与版本的 callback 表、用户上下文和输出句柄。重复名称返回 `SERVICE_EXISTS`。Runtime 复制 callback 表，上下文由扩展持有。callback 参数只在调用期间有效，异步任务需自行复制。

注册、注销、请求和取消 callback 在 REAPER 主线程执行。工作线程可以调用完成和事件接口，它们复制 JSON 并排队，由原生定时器投递。请求 ID 为零表示 send。invoke 需在返回 `REAWEB_OK` 后同步或异步完成一次。非零返回值表示分发失败。事件的窗口 ID 为零表示广播，否则只投递到指定窗口。

callback 应及时返回。不依赖 REAPER 的耗时工作放入扩展工作线程。Runtime 限制每次定时调度的投递量，无法抢占扩展 callback。

卸载扩展前应注销服务并停止、等待工作线程。可选 `on_cancel` 处理超时、关闭、重载和注销。已注销注册项不保留 callback。扩展必须保证 callback 执行期间上下文和模块仍有效，不得在 callback 内卸载模块，且必须在 ReaWebAPI 卸载前停止使用其函数指针。重复注销安全，迟到结果返回 `REQUEST_GONE` 或 `EXTENSION_UNLOADED`。

错误包括 `SERVICE_NOT_FOUND`、`METHOD_NOT_FOUND`、`INVALID_ARGUMENT`、`EXTENSION_UNLOADED`、`MAIN_THREAD_REQUIRED`、`TIMEOUT`、`SERVICE_EXISTS`、`QUEUE_LIMIT`、`SERVICE_ERROR`。原生入口返回状态码，JS invoke 通过 `error.code`、`error.message` 拒绝。

## 状态事件

统一 Monitor Manager 按订阅启停，同一监测源由多个 WebView 共享。优先响应选轨、轨道、标记和播放状态 callback，用 100 ms 原生轮询补充无通知的变化。轨道和标记扫描每次最多处理 64 项，遵守 Runtime 时间预算。事件描述当前状态，允许合并，不是完整编辑日志。

| 事件 | 内容 |
| --- | --- |
| `trackSelectionChanged` | 比较完整有序选轨集合，包括空选区、多选和首条不变的集合替换。返回数量与失效通知。 |
| `trackStateChanged` | 轨道列表及顺序、名称、颜色、mute、solo、arm、TCP/MCP 可见状态。返回数量与失效通知。 |
| `transportChanged` | 播放状态、播放/暂停/录制标记、播放位置、编辑光标、播放速率、循环开关。 |
| `projectChanged` | 活动工程、工程列表、路径、dirty、修改计数和载入代次。覆盖打开、关闭、切换和另存路径变化。 |
| `markersChanged`、`regionsChanged` | 数量与失效通知，比较位置、长度、名称、编号和颜色。 |
| `currentRegionChanged` | 当前 Region 的编号、起止位置、名称和颜色，或 `null`。播放时使用播放位置，否则使用编辑光标。 |
| `loopPointsChanged`、`timeSelectionChanged` | 以秒为单位的 `start`、`end`。 |

新事件共用 `projectEpoch`、`revision`、`available`。订阅后提供初始快照，无变化不增加 revision。失效通知包含 `count`、`invalidated: true`，不携带 FX、路由、State Chunk 或完整列表。通过 `CountSelectedTracks`、`GetSelectedTrack`、`GetTrackName`、`GetMediaTrackInfo_Value`、`EnumProjectMarkers3` 等现有 Mirror 按需读取。工程 ID 是会话内不透明标识，不是 Mirror 句柄。完整字段和控制 API 对照见 [English contract](native-services.md)。

现有旧事件、Clipboard、Drag & Drop、Window/Dock/Focus、System 与设备查询保留原实现和接口。高频二进制 Stream 不属于本版本。

## 验证

`BUILD_TESTING=ON` 包含 Service 生命周期、错误、工作线程完成、状态变化、JS 路由与 Runtime 回归测试。`native_service_extension` 可在 Windows、macOS、Linux 构建。实际 REAPER 验证使用 [Native Service Demo](../web/native-service/README.md)，当前记录见 [VALIDATION](VALIDATION.md)。


## Stream 生命周期与输入

`ReaWeb_SetServiceInput(handle, method)` 将 send 方法注册为主线程即时输入回调。不超过 4096 字节的请求可先于普通命令执行。回调只更新最新输入状态，不执行 I/O 或等待工作线程。`ReaWeb_SetServiceShutdown(handle, callback)` 在移除服务或销毁 Runtime 前调用，生产者应在此停止并等待线程退出。这两个新增入口保持 Service ABI 1。参见 [Native Stream](native-streams.zh-CN.md)。
