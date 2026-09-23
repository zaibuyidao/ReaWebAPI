# Runtime API 完整清单 / Complete API inventory

JavaScript Runtime 只公开以下 **14 个命名空间、65 个方法和 1 个 Promise 属性**。730 项 REAPER Mirror 保持原始根级名称、参数顺序、Promise 和返回值规则。

The JavaScript Runtime exposes only these fourteen namespaces: 65 methods and one Promise property. The 730 REAPER Mirror functions keep their original root-level names and signatures. Exact parameter and result types, including overloads, are in [runtime-api.d.ts](../runtime/runtime-api.d.ts). Behavior and limits: [中文](runtime-api.zh-CN.md) · [English](runtime-api.md) · [Host services](host-api.md).

Lua bootstrap is separate: `reaper.ReaWeb_Open(path)` runs before the browser exists. Lua-only native entry points remain documented in [Host API](host-api.md#lua-entry-points). They are not JavaScript aliases. Native transport command names in `capabilities.methods` are diagnostic wire identifiers, not callable JavaScript property paths; use `capabilities.runtime.namespaces` and this inventory for the public SDK surface.

Lua 通过原生函数 `reaper.ReaWeb_Open(path)` 启动页面。JavaScript 使用本清单中的 Runtime 命名空间。内部通信命令是桥接协议的一部分。

全部 14 个命名空间已有具体能力；`capabilities.runtime.reservedNamespaces` 现在为空数组。`app` 提供当前应用信息，`dragDrop` 提供原生文件／文本拖放；大小写统一为 `dragDrop`，没有 `dragdrop` 别名。

All fourteen namespaces have implemented members. App identity is shared with browser storage. Manifest metadata is read at App creation, and the validator checks complete manifests.

## reaper.host

Lua 后端与 WebView UI 之间的消息。Text messages between a Lua backend and the current WebView document.

| API | 返回 / Result | 用途 |
| --- | --- | --- |
| `reaper.host.send(message)` | `Promise<boolean>` | Queue a string or JSON-serialized value for Lua `ReaWeb_Receive`. Receive Lua text through `events.on('message', callback)` |

## reaper.window

窗口的创建、几何、停靠、可见性、焦点和键盘策略。Window creation and presentation; external OS links belong to system.

| API | 返回 / Result | 用途 |
| --- | --- | --- |
| `reaper.window.open(path)` | `Promise<number>` | 打开本地 HTML，返回新窗口 ID |
| `reaper.window.openDev(url)` | `Promise<number>` | 打开受信任的本机 HTTP 开发服务器 |
| `reaper.window.getSize()` | `Promise<Size>` | 读取当前窗口外部尺寸及模式、单位 |
| `reaper.window.setSize(width, height)` | `Promise<ReaWebBounds>` | 设置浮动窗口外部尺寸 |
| `reaper.window.getPosition()` | `Promise<Position>` | 读取当前窗口位置及模式、单位 |
| `reaper.window.setPosition(x, y)` | `Promise<ReaWebBounds>` | 设置浮动窗口位置 |
| `reaper.window.show()` | `Promise<ReaWebWindowState>` | 显示当前窗口 |
| `reaper.window.hide()` | `Promise<ReaWebWindowState>` | 隐藏当前窗口 |
| `reaper.window.getState()` | `Promise<ReaWebWindowState>` | 读取 ID、标题、停靠、可见、焦点、键盘策略和 `iconVisible` |
| `reaper.window.setTitle(title)` | `Promise<boolean>` | 显式覆盖 HTML 标题，重载后仍保留 |
| `reaper.window.setIcon(path)` | `Promise<boolean>` | 显式设置当前窗口会话图标，覆盖 favicon 自动同步，重载后保留。支持 PNG、ICO、SVG，相对路径基于 App 根目录 |
| `reaper.window.setIconVisible(visible)` | `Promise<boolean>` | 控制标题栏图标及占位，保留图标数据，默认显示。停靠切换、原生窗口重建和重载后保留，Linux 效果取决于窗口管理器 |
| `reaper.window.focus()` | `Promise<boolean>` | 聚焦当前窗口 |
| `reaper.window.setDocked(docked)` | `Promise<boolean>` | true 停靠、false 取消停靠；返回实际停靠状态，取消停靠成功返回 false |
| `reaper.window.isDocked()` | `Promise<boolean>` | 读取停靠状态 |
| `reaper.window.setKeyboardCapture(capture)` | `Promise<boolean>` | 设置键盘捕获策略 |
| `reaper.window.close()` | `Promise<boolean>` | 请求清理并关闭当前窗口 |
| `reaper.window.reload()` | `Promise<boolean>` | 请求清理并重新加载当前页面 |

## reaper.theme

REAPER 主题语义颜色、CSS 变量和跟随。REAPER theme colors and following.

订阅主题变化使用 `reaper.events.on('theme-changed', callback)`。Subscribe to theme changes through this event.

| API | 返回 / Result | 用途 |
| --- | --- | --- |
| `reaper.theme.getColors()` | `Promise<ReaWebTheme>` | 读取语义颜色及 CSS 变量 |
| `reaper.theme.apply(element?)` | `Promise<ReaWebDispose>` | 应用并跟随主题；取消时恢复原有内联值 |

## reaper.dialog

原生选择对话框。Native selection dialogs; selected files are read/written through fs.

| API | 返回 / Result | 用途 |
| --- | --- | --- |
| `reaper.dialog.openFile(options?)` | `Promise<string or null>` | 原生打开文件对话框 |
| `reaper.dialog.saveFile(options?)` | `Promise<string or null>` | 选择保存路径，不执行写入 |
| `reaper.dialog.selectFolder(options?)` | `Promise<string or null>` | 原生目录选择；取消统一返回 null |

## reaper.events

REAPER 工程、对象、播放及窗口状态通知。Host state notifications; document teardown is handled by lifecycle.

| API | 返回 / Result | 用途 |
| --- | --- | --- |
| `reaper.events.on(name, callback)` | `Promise<ReaWebDispose>` | 订阅宿主事件，返回异步取消订阅函数 |
| `reaper.events.off(name, callback)` | `Promise<void>` | 按原回调引用取消该事件的全部匹配订阅；不存在时无操作 |

## reaper.lifecycle

文档就绪、关闭、重载与资源清理通知。Document readiness and cleanup; edit grouping belongs to transaction.

| API | 返回 / Result | 用途 |
| --- | --- | --- |
| `reaper.lifecycle.ready` | `Promise<Readonly<Capabilities>>` | 就绪属性，不能当函数调用；API 调用自动等待握手 |
| `reaper.lifecycle.on(name, callback)` | `Promise<ReaWebDispose>` | 订阅 `before-close`、`before-reload` 或 `cleanup`，均在文档销毁前运行 |

## reaper.debug

日志、错误诊断、开发工具和原生输出缓冲区调试设置。Logging, diagnostics, inspector and native output-buffer tuning.

| API | 返回 / Result | 用途 |
| --- | --- | --- |
| `reaper.debug.log(...values)` | `Promise<boolean>` | 记录信息日志 |
| `reaper.debug.warn(...values)` | `Promise<boolean>` | 记录警告日志 |
| `reaper.debug.error(...values)` | `Promise<boolean>` | 记录错误日志 |
| `reaper.debug.inspect(value)` | `Promise<boolean>` | 记录支持循环引用的调试预览 |
| `reaper.debug.getLogs()` | `Promise<ReaWebLogEntry[]>` | 读取当前窗口最近日志 |
| `reaper.debug.getDiagnostics()` | `Promise<ReaWebDiagnostics>` | 读取后端、状态、错误和队列诊断 |
| `reaper.debug.openDevTools()` | `Promise<boolean>` | 请求显示 [DevTools](devtools.zh-CN.md)，macOS 原生控制不可用时返回 `DEVTOOLS_UNAVAILABLE` 错误 |
| `reaper.debug.setBufferSize(bytes)` | `Promise<number>` | 设置当前文档固定原生输出缓冲区容量 |

## reaper.fs

本地文件及目录数据读写。Local file/directory I/O; no clipboard methods.

| API | 返回 / Result | 用途 |
| --- | --- | --- |
| `reaper.fs.readText(path)` | `Promise<string>` | 读取 UTF-8 文本 |
| `reaper.fs.writeText(path, text, options?)` | `Promise<{ path, bytes }>` | 写入 UTF-8 文本 |
| `reaper.fs.readBinary(path)` | `Promise<Uint8Array>` | 读取二进制字节 |
| `reaper.fs.writeBinary(path, bytes, options?)` | `Promise<{ path, bytes }>` | 写入二进制字节 |
| `reaper.fs.readFile(path, options?)` | `Promise<string or Uint8Array>` | 按 encoding 读取；默认 utf8 |
| `reaper.fs.writeFile(path, data, options?)` | `Promise<{ path, bytes }>` | 按 encoding 写入；Uint8Array 必须指定 binary |
| `reaper.fs.stat(path)` | `Promise<ReaWebFileInfo>` | 查询文件或目录属性 |
| `reaper.fs.readDirectory(path)` | `Promise<ReaWebDirectoryEntry[]>` | 列举目录 |
| `reaper.fs.makeDirectory(path, options?)` | `Promise<boolean>` | 创建目录，可设置 recursive |

## reaper.audio

音频元数据、波形、轨道电平及连续混音控制。Host audio helpers, not a duplicate of the standard REAPER Mirror.

| API | 返回 / Result | 用途 |
| --- | --- | --- |
| `reaper.audio.getFileInfo(path)` | `Promise<ReaWebAudioFileInfo>` | 读取音频元数据 |
| `reaper.audio.getWaveform(path, options?)` | `Promise<ReaWebWaveform>` | 读取各声道 min/max 波形包络 |
| `reaper.audio.getTrackMeter(track)` | `Promise<ReaWebTrackMeter>` | 读取各声道瞬时峰值及 dBFS |
| `reaper.audio.setTrackValueLatest(track, key, value)` | `Promise<{ applied, superseded }>` | 合并连续音量、声像、静音、Solo 写入；不会自动创建 Undo |

## reaper.clipboard

系统剪贴板数据读写，目前仅文本。OS clipboard I/O; text only at present.

| API | 返回 / Result | 用途 |
| --- | --- | --- |
| `reaper.clipboard.readText()` | `Promise<string>` | 读取系统剪贴板文本 |
| `reaper.clipboard.writeText(text)` | `Promise<boolean>` | 写入系统剪贴板文本 |

## reaper.dragDrop

系统级文件／文本复制拖放。Native copy drag/drop; starting a drag requires holding the left mouse button.

接收原生 Drop 使用 `reaper.events.on('native-drop', callback)`，payload 为 files/text/x/y。Receive native drops through this event.

| API | 返回 / Result | 用途 |
| --- | --- | --- |
| `reaper.dragDrop.startFiles(paths)` | `Promise<boolean>` | 发起 1–256 个本地文件的原生复制拖拽；取消返回 false |
| `reaper.dragDrop.startText(text)` | `Promise<boolean>` | 发起原生文本复制拖拽；取消返回 false |

## reaper.app

当前应用的身份、Manifest 元数据和数据目录。Current App identity, metadata and data paths.

| API | 返回 / Result | 用途 |
| --- | --- | --- |
| `reaper.app.getId()` | `Promise<string>` | 与现有存储隔离相同的稳定 App ID |
| `reaper.app.getName()` | `Promise<string>` | app.json 的 name；缺省为入口目录名 |
| `reaper.app.getVersion()` | `Promise<string or null>` | app.json 的 version；缺省为 null |
| `reaper.app.getRootPath()` | `Promise<string>` | 本地入口／资源根目录的绝对路径 |
| `reaper.app.getDataPath()` | `Promise<string>` | 当前 App 专属数据目录，位于 REAPER 资源目录内 |

## reaper.system

运行环境能力发现及操作系统集成。Runtime capability discovery and OS integration.

| API | 返回 / Result | 用途 |
| --- | --- | --- |
| `reaper.system.openExternal(url)` | `Promise<boolean>` | 由系统默认程序打开 http/https/mailto |
| `reaper.system.getCapabilities()` | `Promise<ReaWebCapabilities>` | 查询版本、Mirror 可用性、命名空间和限制 |

| `reaper.system.getPlatform()` | `Promise<ReaWebPlatform>` | windows、macos 或 linux |
| `reaper.system.getArchitecture()` | `Promise<ReaWebArchitecture>` | 扩展进程架构：x64、arm64、x86、arm、unknown |
| `reaper.system.revealInFileManager(path)` | `Promise<boolean>` | 在文件管理器定位已有本地文件／目录；Linux 可能退回打开父目录 |

## reaper.transaction

批处理及托管 Undo 编辑分组。Batching and managed Undo grouping; no database-style atomicity, rollback or isolation guarantee.

| API | 返回 / Result | 用途 |
| --- | --- | --- |
| `reaper.transaction.batch(callsOrBuilder, options?)` | `Promise<unknown[]>` / `Promise<ReaWebBatchResult<T>>` | 数组或同步 Mirror Builder，1–128 项已审核调用，支持引用、解构和 undoLabel |
| `reaper.transaction.beginUndo(label)` | `Promise<string>` | 开始托管编辑手势，返回本页 token |
| `reaper.transaction.endUndo(token)` | `Promise<boolean>` | 结束托管编辑手势 |
| `reaper.transaction.withUndo(label, callback)` | `Promise<T>` | 执行回调并在 finally 结束 Undo，保留回调返回值 |

## 使用 / Usage

JavaScript 通过 `reaper.lifecycle.ready` 等待就绪，再调用标准 REAPER API 和 Runtime 接口。

Await `reaper.lifecycle.ready` before using REAPER APIs and Runtime services.

```js
await reaper.lifecycle.ready;
const track = await reaper.GetTrack(0, 0); // REAPER Mirror
if (track) {
  await reaper.transaction.withUndo("Set volume", async () => {
    await reaper.audio.setTrackValueLatest(track, "D_VOL", 0.5);
  });
}
await reaper.fs.writeText("settings.json", "{}", { overwrite: true });
const childId = await reaper.window.open("other/index.html");
```

`Size` / `Position` are the relevant fields of `ReaWebBounds` plus `mode` and `units`; `Capabilities` is `ReaWebCapabilities` plus `windowId` and `projectEpoch`. `string or null` / `string or Uint8Array` in the table denote unions; the declarations enforce overload-specific results. Except `reaper.lifecycle.ready`, every member is a method returning a Promise. Returned disposer functions also return Promises.
