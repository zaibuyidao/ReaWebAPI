# 宿主 API 参考

具名 C/C++ 服务通过 `reaper.host.service(name)` 访问 [Native Service 注册机制](native-services.zh-CN.md)。`reaper.host.send(message)` 保留 Lua Backend 路由。原生服务另有执行超时和生命周期取消机制。

[Runtime API 完整清单 / Complete inventory](runtime-api-inventory.md) · [TypeScript](../runtime/runtime-api.d.ts)

[English](host-api.md) | **简体中文** · [开发指南](development.zh-CN.md)

这些接口由 ReaWebAPI 提供，与 [730 项 REAPER API](api-reference.md) 分开。JavaScript 窗口方法作用于当前 WebView 文档，不传窗口 ID，所有方法返回 Promise。准确类型见 `reaper.d.ts`。

## 启动与能力

`reaper.lifecycle.ready` 每个文档只等待一次握手，没有固定启动延时。完成后再次等待它不会向 REAPER 发起请求。原生窗口创建后立即返回 ID，Windows 和 macOS 首次显示另行等待页面加载和原生首帧检查，不等待异步业务数据。握手就绪不代表窗口已经可见。优先发起界面数据查询，独立调用可并行进行。

小型独立请求和有大小限制的响应使用快速路径，原生 API 仍由主线程调度执行。大消息与文件 I/O 交给工作线程。实际响应速度仍取决于 REAPER 调度及 WebView 的运行状态。

| 成员 | 完成后的值 | 说明 |
| --- | --- | --- |
| `reaper.lifecycle.ready` | 能力信息及 `windowId`、`projectEpoch` | Promise 属性，不是函数。普通方法也会自动等待它 |
| `reaper.system.getCapabilities()` | `ReaWebCapabilities` | 版本、协议、注册方法、可用原生函数、事件及限制 |
| `reaper.debug.getDiagnostics()` | `ReaWebDiagnostics` | 浏览器后端及版本、窗口状态、生命周期、文档/工程代次、队列计数、最近错误和调度预算 |

`capabilities.api` 包含 `schemaVersion`、`reaperVersion`、`catalogueHash`、`official`、`implemented`、`compatible`、`partial`、`missing`、`available`、`availableMethods`、`unavailable` 和逐项 `bindings`。`methods` 同时包含宿主及标准 API，已注册不代表旧版 REAPER 一定提供相应原生函数，实际可用性应查 `api.availableMethods`。

`projectScope` 为 `all`，`limits` 报告请求字节数、批处理及等待调用上限。诊断计数针对当前窗口，`lastError` 是诊断字符串，不是所有调用错误的历史记录。2 ms 调度预算是软预算，不约束单个原生函数的执行时间。

能力与诊断都包含 `webRuntime`：`contract`（1）、`mode`（`app-virtual` / `dev-http`）、`appId`、`origin`、`storageIsolation`（`origin`）、`localResources`。详见 [Web Runtime 约定](frontend.zh-CN.md)。

## 窗口

| 方法 | 完成后的值 | 行为 |
| --- | --- | --- |
| `reaper.window.openDev(url)` | 数字窗口 ID | 受信任的 loopback HTTP 开发服务器，参见[前端资源约定](frontend.zh-CN.md) |
| `reaper.window.open(path, options?)` | 数字窗口 ID | 打开本地 HTML，相对路径从调用页面所在目录解析 |
| `reaper.window.close()` | `boolean` | 请求关闭，文档销毁可能使尚未完成的 Promise 被拒绝，包括关闭请求本身 |
| `reaper.window.focus()` | `boolean` | 聚焦当前窗口 |
| `reaper.debug.openDevTools()` | `boolean` | 幂等请求显示 DevTools。macOS 原生控制不可用时以 `DEVTOOLS_UNAVAILABLE` 错误拒绝 Promise。详见 [DevTools](devtools.zh-CN.md) |
| `reaper.window.setTitle(title)` | `boolean` | 标题为 1–256 UTF-8 字节，不含 NUL。覆盖本窗口的[网页标题](runtime-api.zh-CN.md#窗口标题)，重载后仍保留 |
| `reaper.window.setIcon(path)` | `boolean` | 在当前窗口会话中覆盖 HTML favicon，重载后保留。本地 PNG、ICO 或 SVG，相对路径基于 App 根目录，也支持绝对路径。详见[窗口图标](runtime-api.zh-CN.md#窗口图标) |
| `reaper.window.setIconVisible(visible)` | `boolean` | 显示或隐藏标题栏图标及占位，保留图标数据。默认 `true`，重载后保留。Linux 装饰支持取决于窗口管理器 |
| `reaper.window.setDocked(docked)` | `boolean` | 返回实际停靠状态，保留页面 |
| `reaper.window.isDocked()` | `boolean` | 获取停靠状态 |
| `reaper.window.setKeyboardCapture(capture)` | `boolean` | 默认 `true`，设为 `false` 后遵循 REAPER 的全局快捷键规则 |
| `reaper.window.setDawKeyboard(enabled)` | `boolean` | 动态覆盖当前 App 或 JSFX 页面的 Space 路由，导航或重载后恢复 JSON 配置，不回写文件 |
| `reaper.window.getState()` | `ReaWebWindowState` | `id`、`title`、`docked`、`visible`、`focused`、`keyboardCapture`、`dawKeyboard`、`iconVisible`、`frameless`、`resizable`、`framelessActive`、`transparent`、`transparentActive`, `cornerRadius`, `cornerRadiusActive` |
| `reaper.window.setFrameless(value)` | `ReaWebWindowState` | 动态显示或隐藏原生标题栏，保留同一窗口 |
| `reaper.window.setCornerRadius(radius)` | `ReaWebWindowState` | 原生圆角半径 `0..64`，停靠和最大化时暂停裁剪 |

窗口位置和停靠状态按入口文件及实例序号保存，最多同时打开 32 个窗口。同一 App 目录的窗口共享浏览器存储，不同目录隔离；句柄、订阅及等待请求始终只属于本页。

## 事件

`reaper.events.on(name, callback)` 完成后返回异步取消订阅函数，可重复调用。回调会收到初始快照及合并后的更新，异步回调中的异常需自行捕获。

| 事件 | 回调数据 |
| --- | --- |
| `projectchange` | `{ projectEpoch, changeCount }` |
| `selectionchange` | `{ projectEpoch, revision, count }`，count 是选中轨道数 |
| `itemselectionchange` | `{ projectEpoch, revision, count }`，选中 Item |
| `takeselectionchange` | 同结构，选中 Item 的活动 Take |
| `transportchange` | `{ projectEpoch, available, state?, position?, cursor?, tempo? }` |
| `fxchange` | `{ projectEpoch, available, focused, touched, changeCount }` |
| `windowstatechange` | `ReaWebWindowState` |
| `frame` | `{ sequence, playing?, position?, tempo?, cursor?, lua?, luaAgeMs? }` |

`frame` 和上面这些事件不一样。它由扩展主动推送，不等 Lua，走的是扩展注册给 REAPER 的定时器回调。

最快每 8 毫秒一帧。这个通道的节奏本来就由定时器定，所以这个值只是一个防止突发的下限；下限定得比定时器自己的间隔还大，就会把定时器送来的帧删掉。25 次探针运行实测：定时器间隔中位数 31.7 毫秒，最小值 10.16 毫秒，最小值出现在卡顿 130 到 141 毫秒之后那一次追赶。8 毫秒低于 25 次运行里出现过的所有间隔，所以一帧都不会被删。

所以正常帧率来自定时器，不来自这个值：REAPER 调定时器的间隔约 31.7 毫秒（中位数，不同系统和 REAPER 版本会有差异），每叫醒一次推一帧，页面大约每秒 31 帧。8 毫秒只在某次叫醒比这更快时才起作用，而实测里除了卡顿后的追赶，没有出现过更快的。

其中 `playing`、`position`、`tempo`、`cursor` 由扩展每帧直接从 REAPER 读，所以 Lua 的 `defer` 循环还没轮到下一轮时，这四个值照样是新的。`lua` 是 Lua 最近一次用 `ReaWeb_SetFrame` 写进去的 JSON 文本，一个字节都不改；`luaAgeMs` 表示那份内容是多少毫秒前写的。帧不排队、也没有初始快照，页面只会看到最新的那一帧。某个 REAPER 函数取不到时，只是少那一个字段，帧照发。

分工是这样的：定时器决定什么时候画，Lua 决定画什么。于是 Lua 慢一点或者抖动，只是新内容晚一点到，不会把窗口卡住。

工程变化和原生轨道选择通知在每个主线程调度周期检查。轨道选择保留 100 ms 兜底检查，以覆盖没有通知的修改。工程切换或加载会更新 `projectEpoch`，应据此刷新依赖工程对象的界面。关闭文档会清理订阅。`ReaWeb_Subscribe`、`ReaWeb_Unsubscribe`、`__reawebHello` 和 `__reawebReceive` 是桥接内部实现，不作为应用接口使用。

FX 事件追踪焦点、最后触碰参数及工程 changeCount，用于使 FX 界面缓存失效，不是所有插件参数变化的逐条通知。state 是 REAPER 播放状态位掩码；available 为 false 时其他播放字段可能缺省。选择扫描按帧分段，大型选择通知可能晚于 100 ms。

## 批处理与连续参数

`reaper.transaction.batch(callsOrBuilder, { undoLabel? })` 接受 1–128 个调用，可传入调用数组或同步 Builder 回调。`capabilities.batchMethods` 与 `ReaWebBatchMethod` 列出 456 个已审核接口，覆盖播放状态、工程与对象查询、MIDI、FX、包络、路由、标记、时间映射和格式转换。工程切换、Action 调用、模态对话框、文件读写、资源生命周期操作、手动 Undo/刷新作用区间及音频样本数组不进入批处理。全部 730 项标准 API 仍可单独调用。

一次 RPC 读取播放信息，无需选中轨道：

```javascript
const data = await reaper.transaction.batch(b => {
  const position = b.GetPlayPosition();
  const state = b.GetPlayState();
  const tempo = b.Master_GetTempo();
  return { position, state, tempo };
});
```

调用在主线程连续执行，减少 RPC 往返，但不会冻结音频时钟或设定轮询频率。新增方法仍需当前 REAPER 版本提供。准入按方法及其全部参数模式审核，可能重新扫描文件或创建资源的 getter 不会仅凭名称放行。

延迟引用也可以连接有依赖关系的调用。以下示例要求索引 0 的轨道存在：

```javascript
const data = await reaper.transaction.batch(b => {
  const track = b.GetTrack(0, 0);
  const [, name] = b.GetTrackName(track);
  const volume = b.GetMediaTrackInfo_Value(track, 'D_VOL');
  return { name, volume };
});
```

`b` 保留 Mirror 方法名、参数顺序和可选参数位置。方法返回延迟引用，多返回值按 Lua 顺序支持索引和解构。回调可返回引用，或包含引用及字面量的嵌套普通对象、数组，执行后还原为对应结果。不返回值时，与 `batch(calls)` 一样返回按调用顺序排列的原生结果数组。两种形式的无返回值结果均为 `null`。`ReaWebBatchBuilder` 从 Mirror 推导方法签名，`ReaWebBatchResult<T>` 推导回调结果类型。

回调在握手完成后同步执行。引用只属于当前回调，可作为完整 API 参数传递或返回，不能 await、参与算术运算或当作真实句柄读取。引用对象始终为真，不能据此判断条件或与 `null` 比较。依赖原生结果的分支应先在批处理外读取，例如先用 `await reaper.GetSelectedTrack(0, 0)` 检查选择，再收集需要轨道的调用。未知方法、异步回调、跨 Builder 引用和无效结构在发送前以 `INVALID_ARGUMENT` 拒绝。回调自身的异常原样传播。

Builder 将调用转换为现有 `ReaWeb_Batch` 请求，不新增 RPC 或 Native 层。原生校验和 `BATCH_FAILED` 详情保持不变。

`ValidatePtr` 和 `ValidatePtr2` 可校验空句柄或已失效的对象句柄，并返回 `false`。校验结果不会使后续调用自动跳过。其他工程的句柄仍会被拒绝，其他对象 API 仍要求句柄有效。

批处理仅作用于**当前工程**。任何外部工程句柄或对象都会在写入前被拒绝，包括后面条目中的外部句柄。方法、可用性和不含引用的条目参数会预先校验；依赖前面结果的参数在该条执行前校验。引用序号从零开始，可通过最多八段 `path` 选取数组元素或对象属性。引用必须作为完整的顶层参数，不能向后引用。

```javascript
const results = await reaper.transaction.batch([
  { method: 'GetSelectedTrack', args: [0, 0] },
  { method: 'GetMediaTrackInfo_Value', args: [{ $ref: 0 }, 'D_VOL'] },
  { method: 'SetMediaTrackInfo_Value', args: [{ $ref: 0 }, 'D_VOL', 0.5] }
], { undoLabel: '设置选中轨道音量' });
// 没有选择时，在 setter 执行前失败。多返回值可用 {$ref: 0, path: [1]} 选取。
```

每组同步执行并配对刷新保护；可选非空标签最多 256 UTF-8 字节，对应一个 Undo 分组。无返回值占一个 `null` 位置。false/0 保持原生含义，旧有轨道值 setter 的 false 则报错。执行失败报告 `BATCH_FAILED`，包含 `completed`、`results`、`rolledBack: false`，以及可选的 `cause`、`cleanupError`。已经完成的写入保留，**批处理不是事务**。

`reaper.transaction.beginUndo(label)` 返回本页专属 token，`reaper.transaction.endUndo(token)` 关闭分组。`reaper.transaction.withUndo(label, async () => { ... })` 自动通过 finally 清理。托管手势采用同一组已审核 API 和当前工程限制。所有窗口合计只能有一个手势；其他窗口的工程调用、嵌套分组、批处理、手动 Undo/刷新调用返回 `UNDO_BUSY`。浏览器事件之间不会保持刷新锁。

宿主会在页面重载/关闭、工程切换/加载或 30 秒后关闭分组；结束过期 token 报 `STALE_UNDO`。await 期间其他脚本和用户仍可能编辑工程，因此应保持手势简短。这不是排他锁，也不自动回滚。直接调用原生 Undo 开启的作用区间仍由调用者负责，不受托管清理保护。

`reaper.audio.setTrackValueLatest(track, key, value)` 对 `D_VOL`、`D_PAN`、`B_MUTE`、`I_SOLO` 合并等待值，每组轨道/参数保留一个正在执行及一个最新等待值，最多 128 组。被替代的值返回 `{ applied: false, superseded: true }`。结束手势前应等待所有写入完成。

## 文件与桌面服务

文件操作在工作线程执行，REAPER API 仍在主线程执行。文件操作使用当前用户的权限，**不会将受信任页面限制在页面目录内**。相对路径基于 HTML 所在目录；允许绝对路径和 `..`。Lua 打开的开发服务器窗口以 `<资源目录>/Scripts/` 为基准，JavaScript 打开的开发窗口继承调用页目录。

| 方法 | 返回值 / 选项 |
| --- | --- |
| `reaper.fs.readFile(path)` | UTF-8 字符串；无效 UTF-8 报 `FILE_ENCODING` |
| `reaper.fs.readFile(path, { encoding: 'binary' })` | `Uint8Array` |
| `reaper.fs.writeFile(path, text, { overwrite?: boolean })` | `{ path, bytes }`；默认拒绝覆盖已有文件 |
| `reaper.fs.writeFile(path, bytes, { encoding: 'binary', overwrite?: boolean })` | 写二进制，输入为 `Uint8Array` |
| `reaper.fs.stat(path)` | `{ path, exists, type, size }`；文件大小为字节，其他为 null |
| `reaper.fs.readDirectory(path)` | 排序后的属性数组，额外包含 `name`，最多 4096 项 |
| `reaper.fs.makeDirectory(path, { recursive?: boolean })` | 布尔值，表示是否创建目录 |
| `reaper.fs.moveFile(source, destination, { overwrite?: boolean })` | `{source,path,bytes}`，在工作线程移动文件，默认不覆盖，支持跨卷 |
| `reaper.clipboard.readText()` | UTF-8 文本，无文本时为空字符串 |
| `reaper.clipboard.writeText(text)` | 布尔值 |
| `reaper.system.openExternal(url)` | 布尔值，将 http/https/mailto 交给系统默认程序 |

文件内容和剪贴板文本限制为 16 MiB，剪贴板写入拒绝 NUL。文件先写到同目录临时文件，再通过重命名/链接提交；父目录必须存在。`overwrite: true` 明确允许覆盖。已经开始的 I/O 可能在页面关闭后完成，未收到回复不代表写入失败，不提供删除、递归清理或自动重试。错误包括 `FILE_IO`、`FILE_NOT_FOUND`、`FILE_NOT_DIRECTORY`、`FILE_EXISTS`、`FILE_ENCODING`、`DIRECTORY_LIMIT`、`CLIPBOARD_BUSY`, `CLIPBOARD_ERROR`, `EXTERNAL_OPEN_FAILED`、`HOST_UNAVAILABLE`、`HOST_TIMEOUT`、`INVALID_URL`。外链成功仅表示操作系统接受请求。

## 错误与限制

`reaper.debug.setBufferSize(bytes)` 设置当前文档固定输出缓冲区的默认容量，并返回新值。范围 4 KiB–16 MiB，默认 64 KiB。REAPER `NeedBig` 缓冲区会自动增长，最终结果仍受大小上限约束。

| 限制 | 数值 |
| --- | --- |
| 桥接 JSON 请求 / 结果 | 64 MiB |
| 单个字符串或二进制值 | 16 MiB |
| 音频数组 | 1,048,576 个 double 元素 |
| JavaScript 等待调用 | 256 |
| 单次批处理 | 128 个调用 |
| 每页有效句柄 | 65,536 |
| 执行前排队过期 / 客户端等待保护 | 25 秒 / 30 秒 |

原生调用开始后清除排队等待计时，执行无法抢占。不提供取消或自动重试。页面外部删除对象，也可能使排队调用拿到的句柄在执行前失效。

| 错误码 | 含义及处理 |
| --- | --- |
| `NO_RUNTIME` | 没有原生 WebView 通道，通过 Lua 启动器打开 |
| `PROTOCOL_MISMATCH`、`SCHEMA_MISMATCH` | 桥接或定义不匹配，更新扩展并重开页面 |
| `API_UNAVAILABLE`、`UNKNOWN_API` | 宿主缺少函数或运行时未注册方法，检查能力与名称 |
| `INVALID_ARGUMENT`、`INVALID_REQUEST`、`INVALID_PATH` | 参数、消息或本地入口路径不正确 |
| `INVALID_HANDLE`、`STALE_HANDLE` | 句柄类型错误或对象失效，重新获取对象 |
| `PROJECT_CHANGED`、`DOCUMENT_STALE`、`WINDOW_CLOSED` | 调用环境已变化，刷新状态或重新打开 |
| `QUEUE_LIMIT`、`HANDLE_LIMIT`、`WINDOW_LIMIT` | 等待调用、句柄或窗口过多 |
| `BUFFER_LIMIT`、`MESSAGE_LIMIT`、`ARRAY_CHANGED` | 数据过大、固定缓冲区不足或音频数组长度被改变 |
| `TIMEOUT`、`REQUEST_EXPIRED` | 未及时响应或执行前已过期，重试写入前先检查状态 |
| `RESOURCE_IN_USE` | 试图销毁仍归 Take 使用的 PCM source |
| `BATCH_FAILED`、`UNDO_UNAVAILABLE` | 检查已完成结果，或宿主无法提供所需 Undo 分组 |
| `UNSUPPORTED_PARAMETER`、`UNSUPPORTED_PROJECT` | 参数超出批处理预校验的较窄范围 |
| `UNKNOWN_EVENT`、`DOCK_UNAVAILABLE`、`DOCK_FAILED` | 不支持的事件或停靠失败 |
| `NATIVE_ERROR`、`WRONG_THREAD` | 原生失败或未从 REAPER 主线程调用入口 |
| `ICON_FORMAT`、`ICON_INVALID`、`ICON_LIMIT` | 格式不支持、图像无效或超过图标限制 |
| `ICON_APPLY_FAILED`、`ICON_SUPERSEDED` | 原生图标应用失败，或待处理请求被较新的请求取代 |

错误结构对应 `ReaWebError`。捕获 JavaScript 异常后，先判断其类型再读取扩展字段。原生 `false` / `0` / `null` 仍是普通结果，含义按对应 REAPER API 说明判断。

## Lua 入口

这些原生扩展函数在 Lua 中同步调用，先使用 `reaper.APIExists("ReaWeb_Open")` 检查是否安装。

| Lua 调用 | 返回 |
| --- | --- |
| `reaper.ReaWeb_OpenDev(url)` | 正数窗口 ID，失败返回 0 |
| `reaper.ReaWeb_Open(path, instanceKey?, id?, multiple?)` | 正数窗口 ID，失败为 `0` |
| `reaper.ReaWeb_OpenEx(path, optionsJson)` | 正数窗口 ID，失败为 `0` |
| `reaper.ReaWeb_Show(id)` | 布尔值，显示但不强制聚焦 |
| `reaper.ReaWeb_Close(id)` | 布尔值 |
| `reaper.ReaWeb_IsOpen(id)` | 布尔值 |
| `reaper.ReaWeb_IsReady(id)` | 文档握手完成后为 true |
| `reaper.ReaWeb_Send(id, message)` | 接受当前已就绪文档的消息返回 `true`，失败返回 `false` |
| `reaper.ReaWeb_Receive(id)` | 非阻塞弹出下一条 UTF-8 文本，无消息或失败返回 `""` |
| `reaper.ReaWeb_SetFrame(id, text)` | 存住则 `true`，否则 `false` 并设置 `ReaWeb_GetLastError()`；替换下一帧的 `lua` 字段 |
| `reaper.ReaWeb_Focus(id)` | 布尔值 |
| `reaper.ReaWeb_DevTools(id)` | 布尔值 |
| `reaper.ReaWeb_SetDocked(id, docked)` | 实际停靠状态 |
| `reaper.ReaWeb_IsDocked(id)` | 布尔值 |
| `reaper.ReaWeb_GetDiagnostics(id)` | JSON 字符串，失败为空 |
| `reaper.ReaWeb_GetLastError()` | 最近一次同步入口错误字符串 |

Lua 相对 HTML 路径从 REAPER 的 `Scripts/` 目录解析，模板使用启动器所在目录的绝对路径。正数窗口 ID 表示窗口已创建，不表示页面就绪。异步加载错误可能稍后出现在 REAPER 控制台和诊断信息中，启动器无需 defer 循环维持页面。

[v0.1.8 Runtime namespaces](runtime-api.md) · [v0.1.8 命名空间接口](runtime-api.zh-CN.md)

JavaScript 使用 `reaper.window.open(path, options?)` 和 `reaper.lifecycle.ready`。Lua 在网页尚未启动时通过 `reaper.ReaWeb_Open(path)` 启动窗口；Lua 原生扩展函数独立于浏览器 SDK。730 项标准 REAPER 镜像名称保持不变。

`reaper.events.off(name, callback)` 可按回调引用取消该事件的全部匹配订阅。使用 `reaper.events.on('native-drop', callback)` 订阅原生拖放事件，返回 files/text/x/y，不合并、不提供初始快照。详见 [Runtime API](runtime-api.zh-CN.md)。

### 窗口实例

`ReaWeb_Open(path, instanceKey?, id?, multiple?)` 使用位置参数，不接收 Lua table。从 v0.3.5 起，非空 `instanceKey` 会复用并聚焦匹配的已打开或初始化中的窗口，返回原有数字窗口 ID。复用保留页面状态，忽略本次传入的 HTML 路径。未传入、nil 或空字符串 key 每次都创建新窗口。`multiple = true` 跳过复用，不替换已有单实例。

```lua
local instanceKey = debug.getinfo(1, "S").source
local directory = instanceKey:sub(2):match("^(.*[/\\])")
local window = reaper.ReaWeb_Open(directory .. "index.html", instanceKey)
local settings = reaper.ReaWeb_Open(directory .. "settings.html", instanceKey, "Settings")
local extra = reaper.ReaWeb_Open(directory .. "index.html", instanceKey, nil, true)
```

Runtime 对 `(instanceKey, id)` 两个字符串分别进行区分大小写的精确匹配。nil 或空字符串 `id` 表示默认窗口。实例匹配将 key 作为不透明标识，不做路径规范化，也不移除 source 的 `@` 前缀。App 身份解析在缺少 `app.json.id` 时单独使用 Lua source 的文件名，不读取 Lua 调用栈。在启动脚本中获取 source，再传给需要的辅助函数。后续调用应保持相同写法。复制到不同目录的启动脚本具有不同 key。正在关闭或初始化失败的窗口不参与复用，会话销毁时释放实例身份。

JavaScript 的 `reaper.window.open(path, options?)` 和 `ReaWeb_OpenDev(url)` 行为不变。原生 C/C++ 调用方须使用 v0.3.5 签名 `int ReaWeb_Open(const char* path, const char* instanceKey, const char* id, const bool* multiple)`，省略的选项传 `nullptr`。单参数 Lua 调用要求 App 定义 `app.json.id`。缺少该字段时须将启动脚本 source 作为 `instanceKey`，包括 `multiple = true` 的调用。

## Lua 消息桥接

支持两种开发方式。纯 JavaScript 应用使用 REAPER Mirror 和 Runtime API。Lua 后端使用普通 REAPER/ReaScript API，通过消息与 WebView UI 通信。参见[示例](../web/lua-backend/README.md)。

`ReaWeb_Send(id, message)` 在 `ReaWeb_IsReady(id)` 为真后接受 UTF-8 文本，不序列化 Lua table，也不自动识别 JSON。`reaper.events.on("message", callback)` 接收原始字符串。未订阅时，消息保留在当前文档队列中。消息不合并，不向新监听器回放已投递内容。

`await reaper.host.send(message)` 原样发送字符串，其他 JSON 值先通过 `JSON.stringify` 序列化。不可序列化的值、循环对象、非有限数值和含原始 NUL 的文本以 `INVALID_ARGUMENT` 拒绝。返回 `true` 表示原生队列已接受，不代表 Lua 已处理。Lua 通过 `ReaWeb_Receive(id)` 获取文本。

`ReaWeb_SetFrame(id, text)` 不是发消息。它只替换每个窗口里存着的那一个字符串，所以既不排队，慢页面也不会重放旧内容。扩展会把这份字符串挂到下一次推给页面的帧上。每轮 `defer` 里先比较内容，变了才调用；在更新的内容到来之前，上一份会一直显示。限制是 1 MiB UTF-8 文本、不含 NUL。超长或非法文本返回 `false`，保留上一份内容。失败原因用 `ReaWeb_GetLastError()` 读，是说明文字：超长是 `Frame content exceeds 1 MiB of UTF-8 text`，含 NUL 是 `Frame content cannot contain NUL`，不是合法 UTF-8 是 `Frame content must be valid UTF-8`。

每个窗口的两个方向各自保持 FIFO。单条消息最多 1 MiB UTF-8，每方向最多 256 条待处理消息，窗口双向队列合计最多 16 MiB，包括等待原生输出分发的消息。单条超限返回 `MESSAGE_LIMIT`，队列满返回 `QUEUE_LIMIT`。原有传输限制仍适用，可通过 `capabilities.runtime.host` 查询消息上限。

Lua 延续 `ReaWeb_GetLastError()` 错误约定。未就绪时 Send 失败，错误为 `NOT_READY`。窗口关闭或 ID 无效时为 `WINDOW_CLOSED`。Receive 在无消息或失败时返回 `""`。允许空字符串消息，但其出队结果与空队列无法区分。需要区分时可使用 `null` 等 JSON 文本。文本 ABI 不支持原始 NUL。成功调用不会清除之前的错误。

关闭窗口或导航到新文档会丢弃双向队列，并丢弃 Lua 上次用 `ReaWeb_SetFrame` 存入的帧内容，旧文档的消息和帧内容都不会进入新页面。Host API 必须在 REAPER 主线程调用，不等待消息到达。只有持续接收消息才需要 Lua `defer` 循环，窗口存活不依赖该循环：

```lua
local function loop()
  if not reaper.ReaWeb_IsOpen(id) then return end
  for _ = 1, 32 do
    local text = reaper.ReaWeb_Receive(id)
    if text == "" then break end
    handle_message(text)
  end
  reaper.defer(loop)
end
loop()
```

### 隐藏启动

v0.3.8.15 起，`ReaWeb_OpenEx(path, optionsJson)` 接收 JSON 对象字符串，可选字段为 `instanceKey:string`、`id:string`、`multiple:boolean`、`hidden:boolean`、`frameless:boolean` 、`resizable:boolean` 和 `transparent:boolean`。默认值沿用 `ReaWeb_Open`，`hidden` 和 `frameless` 默认为 `false`，`resizable` 默认为 `true`。`frameless` 和 `resizable` 要求 v0.3.8.16，`transparent` 要求 v0.3.8.18，默认 `false`。未知字段、类型错误、null、空字符串或无效 JSON 返回 `0`，通过 `ReaWeb_GetLastError()` 获取错误。缺省或空身份字符串沿用原有实例规则。

```lua
assert(reaper.APIExists("ReaWeb_OpenEx") and reaper.APIExists("ReaWeb_Show"), "ReaWebAPI v0.3.8.15 required")
local id = reaper.ReaWeb_OpenEx(path, '{"instanceKey":"my-app","hidden":true}')
assert(id > 0, reaper.ReaWeb_GetLastError())
-- 在 App 发出自定义 ui-ready 消息后显示。
assert(reaper.ReaWeb_Show(id), reaper.ReaWeb_GetLastError())
```

隐藏启动允许页面加载、Runtime 握手和 Lua/JS 通信，原生窗口保持不可见且不聚焦。`ReaWeb_Show(id)` 显示窗口但不强制聚焦，`ReaWeb_Focus(id)` 显示并聚焦。macOS 上显式 Focus 还会激活处于后台的 REAPER。无效或已关闭 ID 明确失败。主动 Show/Focus 可以在首帧准备完成前显示窗口，Close 优先于待执行的展示。

`hidden` 仅影响新实例。`hidden:true` 命中相同实例时保留当前可见性和焦点。普通 Open 或 `hidden:false` 的 OpenEx 会显示并聚焦复用的窗口，不重新加载。`multiple:true` 始终新建。

隐藏启动选项不写入持久化状态，重载保留当前显示意图。已有 Docker 位置在后台加载期间保持待接入状态，首次 Show/Focus 时恢复。`isDocked()` 包含此待接入状态，在首次显示前关闭仍保留原停靠配置。此操作不隐藏其他 Docker 标签或 REAPER 主窗口。接入失败会报告错误，可重试，不退化为可见的浮动启动。

`ReaWeb_IsReady` 仅表示文档 Runtime 握手完成，不代表首帧或业务数据已就绪。需要时使用 App 自定义的 `ui-ready` 消息。隐藏页面的定时器和渲染可能受浏览器节流，Show 不保证 GPU 已完成首帧合成。

原生插件通过 `GetFunc` 获取 `ReaWeb_OpenEx` 的 `int (*)(const char*, const char*)` 和 `ReaWeb_Show` 的 `bool (*)(int)`。原有四参数 `ReaWeb_Open` ABI 不变。JavaScript 在传入 `{hidden:true}` 前检查 `(await reaper.system.getCapabilities()).runtime.window?.hiddenStart`。旧版本继续使用普通 Open，或明确报告不支持隐藏启动。

### 无边框窗口

v0.3.8.16 起，Lua `ReaWeb_OpenEx` 和 JavaScript `window.open` 接受 `frameless?:boolean`（默认 `false`）及 `resizable?:boolean`（默认 `true`）。两者设定初始配置。v0.3.9.2 起可通过 `window.setFrameless(boolean)` 更改当前会话的标题栏配置。复用实例保留原配置和文档，Reload 与 Dock/Undock 保留配置，两个字段不写入 WindowState。`resizable:false` 禁止交互式缩放及最大化，合法的 `setSize()` 调用和 Docker 布局仍然可用。原生 ABI、外部尺寸及原生坐标单位不变。

```lua
local id = reaper.ReaWeb_OpenEx(path, '{"instanceKey":"my-app","frameless":true,"resizable":true}')
assert(id > 0, reaper.ReaWeb_GetLastError())
```

```javascript
const id = await reaper.window.open('other/index.html', {frameless:true, resizable:false});
```

`getState()` 和 `windowstatechange` 增加 `frameless`、`resizable` 与 `framelessActive`。`frameless` 表示当前会话配置，`resizable` 表示创建配置，`framelessActive` 仅在独立无边框宿主内为 `true`。所有 Docker、浮动 Docker 和待恢复的保存停靠状态均为 `false`。Dock/Undock 保留 WebView 文档、句柄和订阅，原生标题和图标 API 的含义不变。

使用 `data-reaweb-drag` 与 `data-reaweb-no-drag` 声明[原生拖动区](frontend.zh-CN.md#原生拖动区)。`setFrameless(boolean)` 保留宿主、文档、窗口外部尺寸位置、可见状态和停靠关系。客户区随原生标题栏调整，不触发导航。`resizable` 仍仅在创建时设置。[可运行示例](../web/frameless/README.md)提供可缩放与固定尺寸启动脚本，以及使用既有生命周期的关闭按钮。

`capabilities.runtime.window` 和诊断中的 `windowFeatures` 报告 `frameless`、`resizable`、`dragRegions` 及 `windowSystem`（`win32`、`appkit` 或 `x11`）。Windows 要求 WebView2 非客户区支持，Linux 要求 X11/XWayland 和支持 `_NET_WM_MOVERESIZE` 的窗口管理器，不支持原生 Wayland。不支持的创建选项明确失败，异步 WebView 初始化失败沿用既有错误报告和清理流程，不退化为带装饰的可见窗口。

### 动态外观

v0.3.9.2 起，`window.setFrameless(boolean)` 和 `window.setCornerRadius(radius)` 返回 `ReaWebWindowState`。配置在当前会话的重载和停靠切换中保留。在 Docker 内调用只记录浮动配置。JSFX 托管编辑器返回 `WINDOW_HOSTED`。

`radius` 接受 `0..64` 的有限数值，单位为逻辑桌面单位，Windows 使用 96 DPI 单位，macOS 使用点，Linux 使用 GDK 单位。`0` 取消自定义裁剪。`getState()` 和 `windowstatechange` 包含 `cornerRadius` 与 `cornerRadiusActive`。停靠或最大化时暂停裁剪，恢复普通浮动后重新应用。原生圆角无需透明页面 CSS。

Windows 的有标题栏与无标题栏窗口均支持自定义半径。macOS 和 Linux 的有标题栏窗口使用系统圆角，切换为无标题栏后应用保存的半径。通过 `capabilities.runtime.window.setFrameless` 和 `cornerRadius.{supported,framed,frameless,max}` 检测能力。Linux 要求 GDK3/X11 窗口形状支持，装饰切换效果取决于窗口管理器。支持小数半径，必要时按原生像素取整。

```javascript
await reaper.window.setCornerRadius(20);
await reaper.window.setFrameless(true);
// 在同一窗口恢复原生标题栏。
await reaper.window.setFrameless(false);
```

网页右键菜单与原生窗口菜单保持一致，提供停靠、重载、DevTools、打开应用文件夹和关闭。浏览器默认项继续隐藏。应用可通过取消 DOM `contextmenu` 事件保留自定义菜单。

## 透明窗口

v0.3.8.18 起，`ReaWeb_OpenEx` 与 `reaper.window.open` 接受 `transparent?:boolean`，默认 `false`，仅在创建窗口时生效。复用实例保留已有配置和文档。旧 `ReaWeb_Open` ABI 不变。

```lua
local id = reaper.ReaWeb_OpenEx(path,
  '{"instanceKey":"my-app","frameless":true,"transparent":true}')
assert(id > 0, reaper.ReaWeb_GetLastError())
```

```js
const id = await reaper.window.open('panel.html', {frameless:true, transparent:true});
```

`getState()` 与 `windowstatechange` 返回请求值 `transparent` 及原生桌面 Alpha 状态 `transparentActive`。后者不依赖页面 CSS，原生初始化尚未完成时可以为 `false`。页面需显式设置透明 CSS，不透明 CSS 仍显示不透明内容。透明像素保留矩形鼠标命中区域。

`capabilities.runtime.window.transparent` 表示接受该选项。实际支持情况通过 `transparency.supported`、`framed`、`frameless`、`backend` 与 `reason` 判断。Windows 使用 HWND WebView2 控制器与 DWM Alpha，要求 Controller2。macOS 使用非不透明 AppKit 宿主及 WKWebView 背景控制。Linux 要求 GDK3/X11、32 位 RGBA 视觉格式、活动合成器，以及 REAPER/SWELL 的 `SWELLGdkAlphaChannel` 支持，不支持原生 Wayland。Linux 隐藏窗口在首次 Show 时建立原生宿主。

透明功能仅适用于独立浮动窗口。首次呈现前临时跳过已保存的 Dock 恢复，继续保存位置和尺寸，并保留原来的 `docked` 与 `dockId` 偏好。关闭后不带 `transparent:true` 重新打开时恢复原 Dock 偏好。透明选项本身不持久化。所有 Dock 请求均以 `DOCK_UNAVAILABLE` 拒绝，包括 Lua 和上下文菜单。对已浮动窗口取消停靠仍保持幂等。

选项类型错误返回 `INVALID_ARGUMENT`，缺少原生支持使用 `TRANSPARENCY_UNAVAILABLE`，Alpha 初始化失败使用 `TRANSPARENCY_FAILED`。Lua 同步失败返回 `0`，通过 `ReaWeb_GetLastError()` 获取原因。JavaScript 沿用 Promise 错误合同。原生异步失败关闭待创建窗口，并写入诊断和日志，正数窗口 ID 不代表已就绪。不提供动态透明切换、鼠标穿透或 Docker 透明。参见[可运行示例](../web/transparent/README.md)。
