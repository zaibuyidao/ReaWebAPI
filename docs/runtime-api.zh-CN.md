# Runtime API

[Runtime API 完整清单 / Complete inventory](runtime-api-inventory.md) · [TypeScript](../runtime/runtime-api.d.ts)

[English](runtime-api.md) | **简体中文**

JavaScript Runtime 使用下列 13 个命名空间。730 项标准 REAPER 镜像的名称、Promise、参数及返回顺序保持不变。入口声明是 `reaper.d.ts`，它同时引用 `reaper-api.generated.d.ts` 和 `runtime-api.d.ts`。这三个文件应一起分发。

`capabilities.runtime.contract` 为 2，描述宿主 SDK；`capabilities.webRuntime.contract` 仍为 1，描述 Web 资源和存储约定。能力信息列出命名空间、事件、清理超时和音频上限；底层标准 API 是否存在仍应查询 `api.availableMethods`。

就绪入口为 `await reaper.lifecycle.ready`。批处理及托管 Undo 统一到 `reaper.transaction`；连续混音控制使用 `reaper.audio.setTrackValueLatest`；能力查询和固定输出缓冲区设置分别使用 `reaper.system.getCapabilities()`、`reaper.debug.setBufferSize(bytes)`。参数、错误与清理约定见[宿主服务参考](host-api.zh-CN.md)。

13 个命名空间均提供具体方法，共 64 个方法和 1 个 Promise 属性；`capabilities.runtime.reservedNamespaces` 为空数组。完整方法和类型见 [API 清单](runtime-api-inventory.md)。

`transaction` 管理批处理和 Undo 分组，不承诺数据库式原子性、回滚或隔离；已完成写入不会自动撤回，其他编辑仍可能交错。方法与类型见[完整清单](runtime-api-inventory.md)。

## FS

`reaper.fs` 提供 `readText`、`writeText`、`readBinary`、`writeBinary`，以及支持 encoding 重载的 `readFile` / `writeFile`、`stat`、`readDirectory`、`makeDirectory`。文本使用 UTF-8，二进制使用 `Uint8Array`。写入可传 `{overwrite:true}`，默认拒绝覆盖已有文件；继续采用 worker 文件操作和 16 MiB 上限。剪贴板文本使用 `reaper.clipboard.readText()` / `reaper.clipboard.writeText(text)`。外部链接使用 `reaper.system.openExternal(url)`。

## Window

```js
await reaper.window.setSize(900, 700);
await reaper.window.setPosition(100, 100);
const size = await reaper.window.getSize();
const position = await reaper.window.getPosition();
await reaper.window.show();
await reaper.window.hide();
```

另有 `open(path)`、`openDev(url)`、`getState`、`setTitle`、`setIcon`、`focus`、`setDocked`、`isDocked`、`setKeyboardCapture`、`close`、`reload`。使用 `setDocked(true)` 停靠、`setDocked(false)` 取消停靠；返回实际停靠状态，取消停靠成功返回 false。创建新窗口返回窗口 ID，其余窗口控制针对当前调用页，不接收 ID。尺寸是包含边框的原生桌面窗口尺寸，坐标是屏幕坐标，返回 `units: 'native'`；不要当作网页 CSS 像素。后端和桌面缩放可能影响它们与 CSS 像素的比例。宽高允许 100–16384，位置允许 -1000000–1000000，系统会将窗口限制到可用屏幕区域。

返回的 `mode` 区分 `floating` 与 `docked`。Docker 布局由 REAPER 控制，停靠时 `setSize/setPosition` 报 `WINDOW_DOCKED`，不会修改 REAPER 主窗口。隐藏只作用于本页容器，显示时激活对应 Docker 标签。窗口支持标题、聚焦、停靠和位置保存。

用户可直接使用原生宿主栏的 **Dock/Undock** 操作，无需页面代码。Windows 标题栏系统菜单还提供 **Dock in REAPER**。这些入口与 `setDocked()` 使用相同的 REAPER Docker 接口，切换时保留当前 WebView 文档。

### 窗口图标

在 `<head>` 中声明 favicon，Runtime 会自动同步到原生宿主窗口：

```html
<link rel="icon" type="image/svg+xml" href="logo.svg" sizes="any">
```

选择最后一个格式受支持且 `media` 匹配的 `rel="icon"` 声明，兼容 `rel="shortcut icon"`。声明、`<base href>` 或媒体查询变化时自动更新。移除所有有效声明后恢复默认窗口图标，不会自动探测 `/favicon.ico`。

支持 PNG、ICO、SVG。URL 没有对应扩展名时需声明 `type`。相对 URL 按 `document.baseURI` 解析，包括 `<base href>` 的影响。HTTP(S)、data 和 blob URL 通过浏览器 `fetch` 加载，遵循 CSP `connect-src` 和 CORS。加载失败保留当前图标，并在 Console 中报告警告。

需要显式覆盖时使用 `setIcon()`：

```js
await reaper.lifecycle.ready;
await reaper.window.setIcon('logo.svg');
```

`setIcon(path)` 设置当前宿主窗口的图标，成功返回 `true`。支持本地 `.png`、`.ico` 和 `.svg` 文件及绝对路径。相对路径按当前 App 根目录解析，即 `reaper.app.getRootPath()`。不接受 URL。文件上限为 4 MiB，解码后的位图尺寸上限为 4096 × 4096。

有效的 `setIcon()` 请求进入队列后，当前文档停止自动同步，即使随后文件加载失败也是如此。重载或导航到新文档后恢复自动同步。无效文件不替换已应用的图标。较新的请求会取代尚未完成的旧请求，旧请求以 `ICON_SUPERSEDED` 拒绝。

两种方式共用相同的大小限制和原生渲染流程。Runtime 在后台线程解码，根据当前显示缩放与原生图标尺寸在内存中栅格化 SVG，并保留源数据供 DPI 变化时重新渲染，不生成临时图片。SVG 应自包含，文字需转换为路径。停靠切换保留当前图标。

Windows 设置窗口大小图标。macOS 使用浮动窗口的文档代理图标，不更改 REAPER 的应用或 Dock 图标。Linux 设置浮动窗口图标，是否显示由桌面主题和窗口管理器决定。停靠标签的显示由 REAPER 控制。`setIcon()` 不修改 HTML 中的 favicon 声明。

## Lifecycle

```js
const stop = await reaper.lifecycle.on('before-close', async () => {
  await reaper.fs.writeFile('settings.json', JSON.stringify(settings), {overwrite:true});
});
await reaper.lifecycle.on('cleanup', () => worker.terminate());
// 不再需要时：await stop();
```

支持 `before-close`、`before-reload`、`cleanup`，均在文档销毁前运行。回调收到 `{reason, timeoutMs}`，可以返回 Promise；相关 before-* 和 cleanup 回调会一起调用并并发等待，总等待上限 2000 ms；需要顺序执行的保存与释放应放在同一回调内。异常或超时不阻止宿主原有关闭/重载及原生资源清理。未注册生命周期监听器的应用直接关闭。

扩展关闭 API、窗口关闭按钮、同页原生导航/重载均接入通知。清理期间桥接仍可用于保存设置；完成或超时后，旧文档的请求、句柄、事件和音频任务失效。普通关闭和重载不能被应用否决。不要在清理回调内再次调用关闭或重载。

进程退出、崩溃或外部强制销毁无法保证异步保存。未经过正常通知的 `pagehide` 只尽力调用同步 cleanup，`reason` 为 `unload`、`timeoutMs` 为 0。重要状态应在正常操作过程中持续保存。宿主继续负责托管 Undo 和原生对象兜底清理。

## Events

```js
const stop = await reaper.events.on('track-added', event => console.log(event.guids));
await stop();
```

JavaScript 通过 `reaper.events.on` 订阅宿主事件。事件只用于更新界面，不是完整编辑历史；忙碌时同名通知可能合并，不能把 GUID 列表当作无遗漏的增量日志。

| 事件 | 含义 |
| --- | --- |
| `track-added` / `track-deleted` | 当前工程轨道 GUID 集合的增减；初次扫描建立基线，不把已有轨道当作新增 |
| `track-selected` | 轨道选择快照，与 `selectionchange` 等价 |
| `item-changed` / `take-changed` | 当前工程内容变化使 Item/Take 缓存失效，`scope: 'project'`；可能包含与该缓存无关的编辑，不返回虚构的逐对象差异 |
| `playback-state-changed` | 播放状态位变化，独立于连续播放位置更新 |
| `tempo-changed` | 当前主速度值变化；不表示整个速度图的逐标记差异 |
| `marker-changed` | REAPER 标记/区域原生通知，提示重新读取列表 |
| `fx-changed` | REAPER FX 链、参数等原生通知，提示刷新；不保证捕获每个插件内部状态变化 |
| `project-loaded` | 当前宿主检测到工程加载后的代次通知；普通标签切换继续使用 `projectchange` |
| `project-saved` | 观察到当前工程文件更新且工程变为已保存状态；排除仅 Undo 序列化及自动备份文件，不承诺每次保存恰好一条 |
| `theme-changed` | 当前主题颜色和 CSS 变量快照 |

原生通知只记录线程安全计数；读取 REAPER 状态和向页面分发均在主线程。没有可靠通知的部分使用按需、分段扫描。原生轨道选择通知和工程变更计数在每个主线程调度周期检查。轨道选择保留 100 ms 兜底检查，以覆盖没有原生通知的修改。其他状态扫描约 100 ms，主题约 500 ms。事件异步送达，实际延迟取决于宿主调度、扫描预算和 WebView 响应。轨道、工程加载和保存是变化通知，没有历史回放；其他事件可提供初始快照。页面关闭/重载会取消订阅，异步回调异常不会破坏其他监听器。

## Dialog and theme

```js
const path = await reaper.dialog.openFile({
  title: '选择音频', filters: [{name:'Audio', extensions:['wav','flac']}]
});
const destination = await reaper.dialog.saveFile({initialPath:'result.json'});
const directory = await reaper.dialog.selectFolder();
const restoreTheme = await reaper.theme.apply();
```

对话框复用标准镜像 `GetUserFileName`，使用 REAPER 提供的原生对话框。取消返回 `null`，错误拒绝 Promise；保存对话框只选择路径，不写文件。`initialPath` 交由 REAPER 对话框解释，建议使用绝对路径。筛选扩展名不带点，`*` 表示所有文件。旧宿主缺少该 API 时返回 `API_UNAVAILABLE`。

`reaper.theme.getColors()` 返回 `{available, colors, cssVariables}`，包括 background/text/highlight/panel/border。`reaper.theme.apply(element?)` 默认作用于根元素，跟随主题变化，取消函数恢复先前的内联 CSS 值。`reaper.events.on('theme-changed', callback)` 仅订阅，不应用样式；缺少主题 API 时提供可用的默认配色并标记 `available:false`。不会修改 REAPER 主题。

```css
body { background:var(--reaper-background); color:var(--reaper-text); }
button { border-color:var(--reaper-highlight); }
```

## Debug

`reaper.debug.log/warn/error(...values)` 写入 REAPER 控制台及本窗口日志，`inspect(object)` 记录有长度限制的对象快照，可处理循环引用。`getLogs()` 返回最近 200 项；`getDiagnostics()` 包含原生错误、请求 ID、文档代次、当前清理阶段和音频任务数。自动记录页面未捕获异常和未处理的 Promise 拒绝，不替换 `console`。

日志不上传到任何服务，也不默认记录全部 API 参数或音频数据。

`reaper.debug.openDevTools()` 在 Windows/Linux 上请求显示检查器。在 macOS 上显示 Safari 指引，并以 `INSPECTOR_MENU` 错误拒绝 Promise。快捷键、布局及 `getDiagnostics()` 的 `devtools` 字段见 [DevTools](devtools.zh-CN.md)。

## Audio

```js
const info = await reaper.audio.getFileInfo('/path/to/audio.wav');
const waveform = await reaper.audio.getWaveform(info.path, {points:1024});
const track = await reaper.GetSelectedTrack(0, 0);
const meter = track ? await reaper.audio.getTrackMeter(track) : null;
```

`getFileInfo` 返回 path、sampleRate、channels、bitDepth、duration、format。duration 单位是秒；压缩或其他无法可靠表达位深的格式返回 `bitDepth:null`。解码能力由运行中的 REAPER 决定，仅接受 1–32 声道的音频，不将 MIDI 文件当作音频。

`getWaveform` 支持 `{points, start, duration}`。points 为 1–8192，默认 1024；start 和 duration 单位为秒，默认覆盖全文件。返回完整文件信息及 start、rangeDuration、points、requestedPoints、peaksPerSecond、data。data 的每项是一条声道，含等长 `min/max` 数组，数值为线性幅度。

横坐标为 `start + i / peaksPerSecond`，依据返回的实际 `points` 绘制，不能假定总能返回请求数量。文件顶层 duration 仍是整个文件长度，rangeDuration 才是请求区间。空文件或越界区间报错。

音频任务持有独立 PCM source，不创建临时轨道或 Item，不编辑工程。所有 REAPER 调用留在主线程，峰值构建按 tick 分步推进，JSON 编码通过现有工作线程完成。同一运行时最多 8 个待处理音频任务，波形任务超时 120 秒；关闭/重载会释放 source 并结束峰值构建。单个 REAPER 解码/峰值调用不可抢占，慢磁盘或解码器仍可能影响主线程。

波形使用 REAPER 的峰值设施，宿主可能生成自己的 `.reapeaks` 文件。本版没有额外的 ReaWebAPI 波形缓存、LUFS、频谱、实时流、DSP 或批量分析系统。峰值不可用时明确报 `AUDIO_PEAKS_UNAVAILABLE`，不返回伪造的静音波形。

`getTrackMeter` 返回 channels、peak、peakDb；每声道为 REAPER 当前峰值，静音的 dB 值为 null。它不是 RMS/LUFS，也不是实时音频流。输入必须是本页有效轨道句柄，删除轨道后返回 `STALE_HANDLE`。错误还包括 `AUDIO_UNSUPPORTED`、`AUDIO_INVALID_DATA`、`AUDIO_TIMEOUT`、`FILE_NOT_FOUND`、`QUEUE_LIMIT`。

## App 与系统信息

五个 App getter 都返回 Promise。`getId()` 复用规范化本地入口目录的存储身份，开发模式按受信任 URL 确定身份。同一 App 的窗口共享 ID 和数据目录；关闭、重开保持不变。移动目录或更换开发 URL 会改变身份。它是本机存储标识，不是发布者指定的全球 UUID；Manifest 不覆盖它。

`getRootPath()` 返回本地入口／资源根目录的绝对路径。开发服务器模式返回本地启动目录，Lua 启动器默认是 REAPER 的 Scripts 目录。`getName()` 读取该目录 `app.json` 的可选 name，缺省为目录名；`getVersion()` 读取可选 version，缺省为 null，不使用扩展版本冒充 App 版本。元数据在创建 App 时读取，已有窗口共享快照，全部关闭后重开才重新读取。文件须为不超过 64 KiB 的 JSON 对象；name 非空白且不超过 256 字符，version 使用 schema 的 `N.N.N[-后缀]` 格式。错误元数据以 `APP_MANIFEST_INVALID` 拒绝打开。完整 Manifest 与入口校验仍由独立校验器负责。

`getDataPath()` 返回自动创建的 `<REAPER resource>/ReaWebAPI/Apps/<appId>/Data`，用于配置、缓存和状态，不放在资源目录或浏览器缓存内。返回本机绝对路径，不保证末尾分隔符。例如：`await reaper.fs.writeText((await reaper.app.getDataPath()) + '/settings.json', '{}', {overwrite:true})`。创建目录失败报 `APP_DATA_UNAVAILABLE`，后续读写沿用文件 API 错误。可信 App 仍可访问其他本地路径，这不是权限沙箱。

`system.getPlatform()` 返回 windows、macos、linux；`getArchitecture()` 返回运行扩展的进程架构 x64、arm64、x86、arm 或 unknown，包括模拟运行时的进程架构。`revealInFileManager(path)` 接受已有本地文件或目录，相对路径基于入口目录。Windows Explorer／macOS Finder 定位目标；Linux 使用 FileManager1 ShowItems，不可用时退回打开父目录。系统接受请求返回 true，不代表文件管理器界面已加载完成。错误包括 `INVALID_PATH`、`FILE_NOT_FOUND`、`REVEAL_FAILED`。

## 原生拖放与取消订阅

统一使用大小写明确的 **`reaper.dragDrop`**，不提供 `dragdrop` 别名。`startFiles(paths)` 发起 1–256 个已有普通文件的原生复制拖拽，相对路径基于入口目录，重复路径去重。`startText(text)` 发起非空文本拖拽，上限 16 MiB UTF-8，不允许 NUL。两者返回 Promise：目标接受复制为 true，取消／拒绝为 false；接受不等于目标已导入或解码成功。不会移动或删除源文件，也不自动向 REAPER 工程插入 Item。

应在 pointerdown／mousedown 中调用，且左键仍按住；预先完成文件选择，不要等到 click 后才调用。没有手势报 `DRAG_GESTURE_REQUIRED`；同一运行时一次只允许一个原生拖拽（`DRAG_BUSY`）。关闭销毁拖拽源，重载使旧页面的 Promise 失效。后端失败报 `DRAG_FAILED`／`HOST_UNAVAILABLE`。

```js
button.addEventListener('pointerdown', event => {
  if (event.button !== 0) return;
  event.preventDefault();
  reaper.dragDrop.startFiles([selectedAudioPath]).catch(error => reaper.debug.error(error));
});
const dropped = payload => console.log(payload.files, payload.text, payload.x, payload.y);
const dispose = await reaper.events.on('native-drop', dropped);
await reaper.events.off('native-drop', dropped);
await dispose(); // off 后仍可安全调用
```

`reaper.events.on('native-drop', callback)` 返回异步 disposer。统一 payload 为 `{files:string[], text:string, x:number, y:number}`；files 是绝对本机路径，接收时允许目录；text 无文本时为空字符串；x/y 是视口 CSS 坐标。不读取文件内容。系统／REAPER 拖拽源必须提供标准文件或文本数据，不解析 REAPER 专有对象格式。有订阅时捕获原生 Drop，无订阅时保留普通 WebView DOM 拖放。Drop 不合并、不提供初始快照、不回放；关闭／重载清理订阅。Windows 需要支持原生附加文件对象的 WebView2，旧版不支持时订阅报 `HOST_UNAVAILABLE`。

`events.off(name, callback)` 返回 `Promise<void>`，移除该事件名下原回调引用的全部匹配订阅，包括尚未注册完成的订阅；其他回调不受影响。未订阅时无操作，已在执行的回调不会被中断。`events.on()` 返回的 disposer 可单独取消订阅。`debug.log` 使用 info 日志级别。

## Manifest 校验与示例

`SDK/app-manifest.schema.json` 定义最小 Manifest。源码执行 `python tools/validate_app.py runtime/runtime-demo/app.json`；独立 SDK 执行 `python tools/validate_app.py runtime-demo/app.json`。校验器只读，检查字段、文件存在及符号链接解析后的目录边界，不启动页面。

本版 Manifest 不参与自动安装或启动；应用继续通过 `reaper.window.open` 打开 HTML。没有 App 管理器、自动更新、安装系统或权限执行机制，未实施的 `permissions` 字段会被校验器拒绝。可信应用仍拥有标准镜像及已有文件服务能力，不能将 Web 资源根目录限制误认为 Native API 权限沙箱。

运行 [Runtime Studio](../runtime/runtime-demo/index.html) 对应目录的 `Open.lua` 可体验窗口、事件、主题、对话框、音频波形、手动电平、诊断和清理状态保存。示例不需要构建。
