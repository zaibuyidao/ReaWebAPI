# 开发指南

[English](development.md) | **简体中文** · [文档入口](README.zh-CN.md)

## 开始一个工具

安装与 REAPER 架构对应的扩展，将整个 SDK 复制到可写的开发目录。在 Action List 加载并运行其中的 `starter/Open.lua`，它会打开同目录的 `index.html`。模板展示当前选中轨道，并提供停靠按钮。

```text
SDK/
  reaper.d.ts
  reaper-api.generated.d.ts
  runtime-api.d.ts
  starter/
    Open.lua
    index.html
    app.js
    style.css
    jsconfig.json
```

这个 JavaScript 模板无需服务器、npm 安装或构建步骤。Lua 调用 `reaper.ReaWeb_Open` 后即可结束，窗口由扩展管理。页面运行于浏览器环境，不是 Node.js。普通浏览器不提供 `reaper.window.reaper`。

资源使用相对路径，入口采用延迟执行的普通脚本 `<script src="app.js" defer></script>`。使用打包器时，应输出适合本地 HTML 入口的文件，并在各平台 WebView 中验证资源、模块及网络请求。`reaper.window.open` 只接受本地 `.html` 或 `.htm` 文件，不能直接打开开发服务器网址。

模板的 `jsconfig.json` 开启 JavaScript 类型检查，不生成编译产物，配置含义见 [TypeScript checkJs 文档](https://www.typescriptlang.org/tsconfig/checkJs.html)。使用 TypeScript 时，包含全部三个 `.d.ts` 文件，并先将 `.ts` 编译为浏览器 JavaScript。SDK 声明的是全局对象，不需要 `import reaper`。

## 调用约定

```javascript
await reaper.lifecycle.ready;
const track = await reaper.GetSelectedTrack(0, 0);
if (track) {
  const [ok, name] = await reaper.GetTrackName(track);
  if (ok) console.log(name);
}
```

所有方法均返回 Promise，包括 setter 和无返回值接口。方法会自动等待初次握手，显式等待 `reaper.lifecycle.ready` 可以集中处理启动错误。准确签名及返回名称见 [API 参考](api-reference.md)，单位、标志位、参数键和 REAPER 行为见每项的官方链接。

| 原生 / Lua 概念 | JavaScript 约定 |
| --- | --- |
| 单返回值 | `await` 后得到标量 |
| 多返回值 | 按 Lua 顺序得到数组，包含适用的原生返回值 |
| 无返回值 | `undefined` |
| 可选输入 | 省略尾部参数，或传 `null` / `undefined` 保留中间位置 |
| 当前工程 | 在 `ReaProject` 参数位传 `0` 或 `null` |
| 原生对象指针 | 声明中对应的类型化句柄或 `null` |
| GUID | 带大括号的 GUID 字符串 |
| RECT | 按 Lua 顺序展开的四个坐标 |
| MIDI / 配置字节 | `Uint8Array`，二进制结果也返回此类型 |
| 音频数组 | `Float64Array` 或 `number[]`，Promise 完成前回写 |

必填输入不会自动补默认值。布尔参数需传布尔值，整数需符合原生类型范围及 JavaScript 安全整数范围。普通数值输入必须有限。文本按 UTF-8 处理并拒绝嵌入 NUL，二进制接口会保留零字节及高位字节。`Float64Array` 保留 IEEE 754 特殊值，普通 `number[]` 输入则要求元素有限。

目录包含标准 C/Lua API，不包含 Lua 运行时辅助函数。使用 DOM/canvas 编写 UI，使用浏览器计时器或宿主事件刷新。没有 `reaper.defer`、`gfx`、Lua `require`、SWS 或第三方扩展方法。浏览器计时器不适合让 REAPER API 承担实时音频处理。

## 可用性、工程与句柄

```javascript
const { api } = await reaper.system.getCapabilities();
if (!api.availableMethods.includes('GetTrackName')) {
  throw new Error('当前 REAPER 没有提供 GetTrackName');
}
const [project] = await reaper.EnumProjects(-1);
if (project) console.log(await reaper.CountTracks(project));
```

`implemented` 是编入扩展的绑定数量，`available` 是当前 REAPER 能解析到的函数数量。目录对应 7.80，完整可用需该版本或更新版本。已知但宿主缺少的函数会拒绝调用并返回 `API_UNAVAILABLE`。

句柄归属于一个 WebView 文档。不要解析 ID、写入持久化设置、从地址构造句柄或跨窗口传递。对象删除、文档重载或出现 `STALE_HANDLE` 后需重新获取。句柄可引用其他已打开工程的对象，但切换当前工程时，旧排队请求会返回 `PROJECT_CHANGED`。此时刷新界面，由用户明确重做写操作。

## 写操作与 Undo

对于支持的操作，可用批处理形成一个同步 Undo 步骤：

```javascript
const track = await reaper.GetSelectedTrack(0, 0);
if (track) {
  await reaper.transaction.batch([
    { method: 'SetMediaTrackInfo_Value', args: [track, 'D_PAN', 0] },
    { method: 'SetMediaTrackInfo_Value', args: [track, 'B_MUTE', 0] }
  ], { undoLabel: '居中并取消轨道静音' });
}
```

[批处理契约](host-api.zh-CN.md#批处理与连续参数) 覆盖 173 个已审核标准 API，支持结果引用和 128 项上限，全部 730 项仍可普通调用。批处理不是事务，中途失败不会撤销已经完成的写入，错误中会报告已完成结果。

跨 await 的连续操作可使用 `reaper.transaction.withUndo` 或 `reaper.transaction.beginUndo` / `reaper.transaction.endUndo`；宿主负责重载、关闭、工程变化和 30 秒超时清理，方法范围与批处理相同。原生 Undo 方法仍可单独调用，但调用者须负责配对，不能依赖页面关闭后的 finally 请求。

滑块可使用 `reaper.audio.setTrackValueLatest`，合并同一轨道和参数的等待值。它不会自动生成 Undo 手势。有依赖的调用按顺序 `await`，独立读取采用有限并发，不要一次排队数千次。

## 二进制与资源释放

```javascript
// 读取 MIDI 字节，不将其转为文本。
const item = await reaper.GetSelectedMediaItem(0, 0);
const take = item ? await reaper.GetActiveTake(item) : null;
if (take && await reaper.TakeIsMIDI(take)) {
  const [ok, bytes] = await reaper.MIDI_GetAllEvts(take);
  if (ok) console.log(bytes.byteLength);
}
```

```javascript
const track = await reaper.GetSelectedTrack(0, 0);
const accessor = track ? await reaper.CreateTrackAudioAccessor(track) : null;
if (accessor) {
  try {
    const channels = 2, frames = 256;
    const samples = new Float64Array(channels * frames);
    const start = await reaper.GetAudioAccessorStartTime(accessor);
    const status = await reaper.GetAudioAccessorSamples(accessor, 48000, channels, start, frames, samples);
    if (status > 0) console.log(samples[0]);
  } finally {
    await reaper.DestroyAudioAccessor(accessor);
  }
}
```

等待期间不要调整、转移或复用音频数组，完成 `await` 后再读取。容量需覆盖声道数、采样帧数，以及峰值 API 额外需要的数据块，具体布局见相应官方说明。

创建的 audio accessor、joystick 及未交给工程的 PCM source，应在用完后显式销毁。页面关闭或重载也会释放仍由本页面持有的资源。将创建的 PCM source 赋给 Take 后，所有权转移到工程，不能直接销毁仍被 Take 使用的源。共享或替换资源前，应阅读对应原生 API 的说明。

## 事件、界面和存储

```javascript
const stop = await reaper.events.on('selectionchange', state => {
  console.log('选中轨道数：', state.count);
});
// 界面组件卸载时：
await stop();
```

订阅会提供初始快照及合并后的变化，工程变化和原生轨道选择通知在每个主线程调度周期检查，轨道选择另保留 100 ms 兜底检查。它们用于刷新界面，不是编辑历史或采样时钟。异步回调需自行处理异常，组件卸载时取消订阅，整个页面关闭时会自动清理本页订阅。

停靠切换会保留页面状态。浏览器 profile 按 App 目录隔离，同一目录的窗口共享存储。来源持久化与存储规则见 [Web Runtime 约定](frontend.zh-CN.md)。保存 GUID 或工具设置，不要保存对象句柄，下次打开时针对正确的工程重新解析引用。浏览器存储和网络行为由各平台 WebView 决定。ReaWebAPI 不提供 Node.js 文件系统或 shell 接口。

## 错误与调试

桥接失败会拒绝 Promise，错误对象包含 `code` 和可选 `details`。REAPER 原生返回的 `false`、`0`、`null` 保持其文档含义，不会自动转成异常。

```javascript
try {
  const track = await reaper.GetSelectedTrack(0, 0);
  if (track) console.log(await reaper.GetTrackName(track));
} catch (error) {
  console.error(error); // 桥接错误带有 code 和可选 details。
}
```

`await reaper.debug.getDiagnostics()` 可查看后端、加载阶段、队列计数和最近宿主错误。Windows/Linux 可调用 `reaper.debug.openDevTools()`，macOS 需在 Safari 开启开发者功能，通过 Develop 菜单检查 REAPER 页面。修改资源后重新打开页面。重载会建立新文档，旧句柄失效。

尚未开始的原生请求在 25 秒后过期，客户端另有 30 秒等待保护。原生执行开始后停止排队计时，JavaScript 无法中断已开始的对话框或渲染。不要自动重试超时写入，应先检查状态。限制及错误码见 [宿主接口](host-api.zh-CN.md#错误与限制)。

## 分发工具

分发 Lua 启动器、HTML、编译后的 JavaScript、CSS 和所需资源，保持相对路径。将模板 Action 描述及窗口标题改为自己的工具名称。类型声明和编辑器配置只供开发使用。用户需要安装 ReaWebAPI 原生扩展，不需要 Python 或编译器。

注明最低 REAPER 和 ReaWebAPI 版本，通过能力查询检查依赖的方法，并在界面解释缺失依赖。发布前检查空工程、没有选择、Unicode 路径、对象删除、工程切换、停靠、反复开关窗口及各目标系统。完整 Demo 可辅助桥接诊断，模拟 ABI 测试通过不代表已验证所有原生 API 对真实工程的影响。

`ReaWebAPI-ReaPack-v<版本>.zip` 包含 `ReaWebAPI.ext`、存放七个原生文件的 `extension/` 和完整的 `demo/` 文件夹，三者同级。SDK ZIP 提供开发资料。

## 现代前端与宿主 I/O

Vite/TypeScript、loopback 开发入口、原生本地资源 fetch与 Worker 示例见[前端资源约定](frontend.zh-CN.md)。统一文件读写、剪贴板和外链见[宿主参考](host-api.zh-CN.md#文件与桌面服务)。
