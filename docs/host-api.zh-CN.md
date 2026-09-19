# 宿主 API 参考

[English](host-api.md) | **简体中文** · [开发指南](development.zh-CN.md)

这些接口由 ReaWebAPI 提供，与 [730 项 REAPER API](api-reference.md) 分开。JavaScript 窗口方法作用于当前 WebView 文档，不传窗口 ID，所有方法返回 Promise。准确类型见 `reaper.d.ts`。

## 启动与能力

| 成员 | 完成后的值 | 说明 |
| --- | --- | --- |
| `ready` | 能力信息及 `windowId`、`projectEpoch` | Promise 属性，不是函数。普通方法也会自动等待它 |
| `ReaWeb_GetCapabilities()` | `ReaWebCapabilities` | 版本、协议、注册方法、可用原生函数、事件及限制 |
| `ReaWeb_GetDiagnostics()` | `ReaWebDiagnostics` | 浏览器后端及版本、窗口状态、生命周期、文档/工程代次、队列计数、最近错误和调度预算 |

`capabilities.api` 包含 `schemaVersion`、`reaperVersion`、`catalogueHash`、`official`、`implemented`、`compatible`、`partial`、`missing`、`available`、`availableMethods`、`unavailable` 和逐项 `bindings`。`methods` 同时包含宿主及标准 API，已注册不代表旧版 REAPER 一定提供相应原生函数，实际可用性应查 `api.availableMethods`。

`projectScope` 为 `all`，`limits` 报告请求字节数、批处理及等待调用上限。诊断计数针对当前窗口，`lastError` 是诊断字符串，不是所有调用错误的历史记录。2 ms 调度预算是软预算，不约束单个原生函数的执行时间。

能力与诊断都包含 `webRuntime`：`contract`（1）、`mode`（`app-http` / `dev-http`）、`appId`、`origin`、`storageIsolation`（`app-profile`）、`localResources`。详见 [Web Runtime 约定](frontend.zh-CN.md)。

## 窗口

| 方法 | 完成后的值 | 行为 |
| --- | --- | --- |
| `ReaWeb_OpenDev(url)` | 数字窗口 ID | 受信任的 loopback HTTP 开发服务器，参见[前端资源约定](frontend.zh-CN.md) |
| `ReaWebOpen(path)` | 数字窗口 ID | 打开本地 HTML，相对路径从调用页面所在目录解析 |
| `ReaWeb_Close()` | `boolean` | 请求关闭，文档销毁可能使尚未完成的 Promise 被拒绝，包括关闭请求本身 |
| `ReaWeb_Focus()` | `boolean` | 聚焦当前窗口 |
| `ReaWeb_DevTools()` | `boolean` | 请求检查器，macOS 通过 Safari Develop 连接 |
| `ReaWeb_SetTitle(title)` | `boolean` | 标题为 1–256 UTF-8 字节，不含 NUL |
| `ReaWeb_SetDocked(docked)` | `boolean` | 返回实际停靠状态，保留页面 |
| `ReaWeb_IsDocked()` | `boolean` | 获取停靠状态 |
| `ReaWeb_SetKeyboardCapture(capture)` | `boolean` | 默认 `true`，设为 `false` 后遵循 REAPER 的全局快捷键规则 |
| `ReaWeb_GetWindowState()` | `ReaWebWindowState` | `id`、`title`、`docked`、`visible`、`focused`、`keyboardCapture` |

窗口位置和停靠状态按入口文件及实例序号保存，最多同时打开 32 个窗口。同一 App 目录的窗口共享浏览器存储，不同目录隔离；句柄、订阅及等待请求始终只属于本页。

## 事件

`ReaWeb_On(name, callback)` 完成后返回异步取消订阅函数，可重复调用。回调会收到初始快照及合并后的更新，异步回调中的异常需自行捕获。

| 事件 | 回调数据 |
| --- | --- |
| `projectchange` | `{ projectEpoch, changeCount }` |
| `selectionchange` | `{ projectEpoch, revision, count }`，count 是选中轨道数 |
| `itemselectionchange` | `{ projectEpoch, revision, count }`，选中 Item |
| `takeselectionchange` | 同结构，选中 Item 的活动 Take |
| `transportchange` | `{ projectEpoch, available, state?, position?, cursor?, tempo? }` |
| `fxchange` | `{ projectEpoch, available, focused, touched, changeCount }` |
| `windowstatechange` | `ReaWebWindowState` |

工程及选择状态约每 100 ms 检查一次。工程切换或加载会更新 `projectEpoch`，应据此刷新依赖工程对象的界面。关闭文档会清理订阅。`ReaWeb_Subscribe`、`ReaWeb_Unsubscribe`、`__reawebHello` 和 `__reawebReceive` 是桥接内部实现，不作为应用接口使用。

FX 事件追踪焦点、最后触碰参数及工程 changeCount，用于使 FX 界面缓存失效，不是所有插件参数变化的逐条通知。state 是 REAPER 播放状态位掩码；available 为 false 时其他播放字段可能缺省。选择扫描按帧分段，大型选择通知可能晚于 100 ms。

## 批处理与连续参数

`ReaWeb_Batch(calls, { undoLabel? })` 接受 1–128 个调用，按顺序返回结果。`capabilities.batchMethods` 与 `ReaWebBatchMethod` 列出 173 个已审核接口，覆盖轨道、Item、Take、MIDI、FX、包络、发送、标记和速度。工程切换、Action 调用、模态对话框、文件读写、手动 Undo/刷新作用区间及音频样本数组不进入批处理。全部 730 项标准 API 仍可单独调用。

批处理仅作用于**当前工程**。任何外部工程句柄或对象都会在写入前被拒绝，包括后面条目中的外部句柄。方法、可用性和不含引用的条目参数会预先校验；依赖前面结果的参数在该条执行前校验。引用序号从零开始，可通过最多八段 `path` 选取数组元素或对象属性。引用必须作为完整的顶层参数，不能向后引用。

```javascript
const results = await reaper.ReaWeb_Batch([
  { method: 'GetSelectedTrack', args: [0, 0] },
  { method: 'GetMediaTrackInfo_Value', args: [{ $ref: 0 }, 'D_VOL'] },
  { method: 'SetMediaTrackInfo_Value', args: [{ $ref: 0 }, 'D_VOL', 0.5] }
], { undoLabel: '设置选中轨道音量' });
// 没有选择时，在 setter 执行前失败。多返回值可用 {$ref: 0, path: [1]} 选取。
```

每组同步执行并配对刷新保护；可选非空标签最多 256 UTF-8 字节，对应一个 Undo 分组。无返回值占一个 `null` 位置。false/0 保持原生含义，旧有轨道值 setter 的 false 则报错。执行失败报告 `BATCH_FAILED`，包含 `completed`、`results`、`rolledBack: false`，以及可选的 `cause`、`cleanupError`。已经完成的写入保留，**批处理不是事务**。

`ReaWeb_BeginUndo(label)` 返回本页专属 token，`ReaWeb_EndUndo(token)` 关闭分组。`ReaWeb_WithUndo(label, async () => { ... })` 自动通过 finally 清理。托管手势采用同一组已审核 API 和当前工程限制。所有窗口合计只能有一个手势；其他窗口的工程调用、嵌套分组、批处理、手动 Undo/刷新调用返回 `UNDO_BUSY`。浏览器事件之间不会保持刷新锁。

宿主会在页面重载/关闭、工程切换/加载或 30 秒后关闭分组；结束过期 token 报 `STALE_UNDO`。await 期间其他脚本和用户仍可能编辑工程，因此应保持手势简短。这不是排他锁，也不自动回滚。直接调用原生 Undo 开启的作用区间仍由调用者负责，不受托管清理保护。

`ReaWeb_SetTrackValueLatest(track, key, value)` 对 `D_VOL`、`D_PAN`、`B_MUTE`、`I_SOLO` 合并等待值，每组轨道/参数保留一个正在执行及一个最新等待值，最多 128 组。被替代的值返回 `{ applied: false, superseded: true }`。结束手势前应等待所有写入完成。

## 文件与桌面服务

文件操作在工作线程执行，REAPER API 仍在主线程执行。文件操作使用当前用户的权限，**不会将受信任页面限制在页面目录内**。相对路径基于 HTML 所在目录；允许绝对路径和 `..`。Lua 打开的开发服务器窗口以 `<资源目录>/Scripts/` 为基准，JavaScript 打开的开发窗口继承调用页目录。

| 方法 | 返回值 / 选项 |
| --- | --- |
| `ReaWeb_ReadFile(path)` | UTF-8 字符串；无效 UTF-8 报 `FILE_ENCODING` |
| `ReaWeb_ReadFile(path, { encoding: 'binary' })` | `Uint8Array` |
| `ReaWeb_WriteFile(path, text, { overwrite?: boolean })` | `{ path, bytes }`；默认拒绝覆盖已有文件 |
| `ReaWeb_WriteFile(path, bytes, { encoding: 'binary', overwrite?: boolean })` | 写二进制，输入为 `Uint8Array` |
| `ReaWeb_Stat(path)` | `{ path, exists, type, size }`；文件大小为字节，其他为 null |
| `ReaWeb_ReadDirectory(path)` | 排序后的属性数组，额外包含 `name`，最多 4096 项 |
| `ReaWeb_MakeDirectory(path, { recursive?: boolean })` | 布尔值，表示是否创建目录 |
| `ReaWeb_ClipboardReadText()` | UTF-8 文本，无文本时为空字符串 |
| `ReaWeb_ClipboardWriteText(text)` | 布尔值 |
| `ReaWeb_OpenExternal(url)` | 布尔值，将 http/https/mailto 交给系统默认程序 |

文件内容和剪贴板文本限制为 16 MiB，剪贴板写入拒绝 NUL。文件先写到同目录临时文件，再通过重命名/链接提交；父目录必须存在。`overwrite: true` 明确允许覆盖。已经开始的 I/O 可能在页面关闭后完成，未收到回复不代表写入失败，不提供删除、递归清理或自动重试。错误包括 `FILE_IO`、`FILE_NOT_FOUND`、`FILE_NOT_DIRECTORY`、`FILE_EXISTS`、`FILE_ENCODING`、`DIRECTORY_LIMIT`、`CLIPBOARD_BUSY`, `CLIPBOARD_ERROR`, `EXTERNAL_OPEN_FAILED`、`HOST_UNAVAILABLE`、`HOST_TIMEOUT`、`INVALID_URL`。外链成功仅表示操作系统接受请求。

## 错误与限制

`ReaWeb_SetBufferSize(bytes)` 设置当前文档固定输出缓冲区的默认容量，并返回新值。范围 4 KiB–16 MiB，默认 64 KiB。REAPER `NeedBig` 缓冲区会自动增长，最终结果仍受大小上限约束。

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

错误结构对应 `ReaWebError`。捕获 JavaScript 异常后，先判断其类型再读取扩展字段。原生 `false` / `0` / `null` 仍是普通结果，含义按对应 REAPER API 说明判断。

## Lua 入口

这些原生扩展函数在 Lua 中同步调用，先使用 `reaper.APIExists("ReaWebOpen")` 检查是否安装。

| Lua 调用 | 返回 |
| --- | --- |
| `reaper.ReaWeb_OpenDev(url)` | 正数窗口 ID，失败返回 0 |
| `reaper.ReaWebOpen(html_path)` | 正数窗口 ID，失败为 `0` |
| `reaper.ReaWeb_Close(id)` | 布尔值 |
| `reaper.ReaWeb_IsOpen(id)` | 布尔值 |
| `reaper.ReaWeb_IsReady(id)` | 文档握手完成后为 true |
| `reaper.ReaWeb_Focus(id)` | 布尔值 |
| `reaper.ReaWeb_DevTools(id)` | 布尔值 |
| `reaper.ReaWeb_SetDocked(id, docked)` | 实际停靠状态 |
| `reaper.ReaWeb_IsDocked(id)` | 布尔值 |
| `reaper.ReaWeb_GetDiagnostics(id)` | JSON 字符串，失败为空 |
| `reaper.ReaWeb_GetLastError()` | 最近一次同步入口错误字符串 |

Lua 相对 HTML 路径从 REAPER 的 `Scripts/` 目录解析，模板使用启动器所在目录的绝对路径。正数窗口 ID 表示窗口已创建，不表示页面就绪。异步加载错误可能稍后出现在 REAPER 控制台和诊断信息中，启动器无需 defer 循环维持页面。
