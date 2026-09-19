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

## 窗口

| 方法 | 完成后的值 | 行为 |
| --- | --- | --- |
| `ReaWebOpen(path)` | 数字窗口 ID | 打开本地 HTML，相对路径从调用页面所在目录解析 |
| `ReaWeb_Close()` | `boolean` | 请求关闭，文档销毁可能使尚未完成的 Promise 被拒绝，包括关闭请求本身 |
| `ReaWeb_Focus()` | `boolean` | 聚焦当前窗口 |
| `ReaWeb_DevTools()` | `boolean` | 请求检查器，macOS 通过 Safari Develop 连接 |
| `ReaWeb_SetTitle(title)` | `boolean` | 标题为 1–256 UTF-8 字节，不含 NUL |
| `ReaWeb_SetDocked(docked)` | `boolean` | 返回实际停靠状态，保留页面 |
| `ReaWeb_IsDocked()` | `boolean` | 获取停靠状态 |
| `ReaWeb_SetKeyboardCapture(capture)` | `boolean` | 默认 `true`，设为 `false` 后遵循 REAPER 的全局快捷键规则 |
| `ReaWeb_GetWindowState()` | `ReaWebWindowState` | `id`、`title`、`docked`、`visible`、`focused`、`keyboardCapture` |

窗口位置和停靠状态按入口文件及实例序号保存，最多同时打开 32 个窗口。即使浏览器存储共享，句柄、订阅及等待请求也只属于本页。

## 事件

`ReaWeb_On(name, callback)` 完成后返回异步取消订阅函数，可重复调用。回调会收到初始快照及合并后的更新，异步回调中的异常需自行捕获。

| 事件 | 回调数据 |
| --- | --- |
| `projectchange` | `{ projectEpoch, changeCount }` |
| `selectionchange` | `{ projectEpoch, revision, count }`，count 是选中轨道数 |
| `windowstatechange` | `ReaWebWindowState` |

工程及选择状态约每 100 ms 检查一次。工程切换或加载会更新 `projectEpoch`，应据此刷新依赖工程对象的界面。关闭文档会清理订阅。`ReaWeb_Subscribe`、`ReaWeb_Unsubscribe`、`__reawebHello` 和 `__reawebReceive` 是桥接内部实现，不作为应用接口使用。

## 批处理与连续参数

`ReaWeb_Batch(calls, { undoLabel? })` 接受 1–32 个 `{ method, args }`，按顺序返回结果。原生执行前会校验全部参数。非空 Undo 标签最多 256 UTF-8 字节，写操作同步执行并配对 UI 刷新保护，提供标签时形成一个 Undo 分组。

| 支持的方法 | `args` |
| --- | --- |
| `CountTracks`、`CountSelectedTracks` | `[0]` 或 `[null]` |
| `GetTrack`、`GetSelectedTrack` | `[0, index]` 或 `[null, index]` |
| `GetTrackName` | `[track]` |
| `GetMediaTrackInfo_Value` | `[track, key]` |
| `SetMediaTrackInfo_Value` | `[track, key, value]` |
| `SetTrackColor` | `[track, color]` |
| `GetAppVersion` | `[]` |

批处理的轨道键限于 `D_VOL`、`D_PAN`、`B_MUTE`、`I_SOLO`、`I_CUSTOMCOLOR`，工程参数仅支持当前工程。构造批处理前先获取句柄，一项不能引用同批前一项的结果。无返回值的原生调用在批处理结果数组中占一个 `null` 位置。执行失败返回 `BATCH_FAILED`，details 包含 `completed`、`results`、`rolledBack: false`，有时还包含 `cleanupError`。已经执行的写入不会自动回滚。

`ReaWeb_SetTrackValueLatest(track, key, value)` 支持 `D_VOL`、`D_PAN`、`B_MUTE`、`I_SOLO`，每个轨道/参数保留一个正在执行的写入及最新等待值。被替代的等待值返回 `{ applied: false, superseded: true }`，已执行值返回 `{ applied, superseded: false }`。最多保留 128 组轨道/参数，不创建 Undo 手势，也不改变普通 setter 的行为。

## 错误与限制

`ReaWeb_SetBufferSize(bytes)` 设置当前文档固定输出缓冲区的默认容量，并返回新值。范围 4 KiB–16 MiB，默认 64 KiB。REAPER `NeedBig` 缓冲区会自动增长，最终结果仍受大小上限约束。

| 限制 | 数值 |
| --- | --- |
| 桥接 JSON 请求 / 结果 | 64 MiB |
| 单个字符串或二进制值 | 16 MiB |
| 音频数组 | 1,048,576 个 double 元素 |
| JavaScript 等待调用 | 256 |
| 单次批处理 | 32 个调用 |
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
