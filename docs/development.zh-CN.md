# 开发指南

[English](development.md) | **简体中文** · [文档入口](README.zh-CN.md)

浏览器标准 WASM 线程须显式开启 [App 跨源隔离](frontend.zh-CN.md#app-级跨源隔离)，并在选择线程构建前检查 `webRuntime.browser`。[示例](../examples/runtime-features/isolation/README.zh-CN.md) 包含 App CSP 和四 Worker 原子计算。

## 构建 API 手册

在仓库根目录执行一次 `npm ci --prefix examples/templates/modern`（Node.js 22.12+），随后按平台配置 CMake。`cmake --build build --config Release` 会生成 `build/api-reference/reawebapihelp.html`。用 `python tools/api_reference.py --output build/api-reference --verify` 校验版本、源码 revision 和文件哈希。

在仓库 Settings → Pages 中选择 GitHub Actions，并允许发布工作流部署到 `github-pages` 环境。首次正式发布后即可访问[在线手册](https://zaibuyidao.github.io/ReaWebAPI/reawebapihelp.html)。部署失败时，可对同一发布提交重新运行工作流。将 ReaPack ZIP 内的 `ReaWebAPI.ext`、`extension/`、`web/` 同步到 `zaibuyidao/ReaScripts` 的 `ReaWebAPI/` 目录，再更新索引。

## 开始一个工具

安装与 REAPER 架构对应的扩展，将整个 SDK 复制到可写的开发目录。在 Action List 加载并运行其中的 `starter/reawebapi-starter.lua`，它会打开同目录的 `index.html`。模板展示当前选中轨道，在页面右键菜单中切换 **Dock <App> in Docker** 的勾选状态以停靠或取消停靠。

```text
SDK/
  reaper.d.ts
  runtime-api.d.ts
  starter/
    reawebapi-starter.lua
    index.html
    app.js
    style.css
    jsconfig.json
```

这个 JavaScript 模板无需服务器、npm 安装或构建步骤。Lua 调用 `reaper.ReaWeb_Open` 后即可结束，窗口由扩展管理。Starter 将启动脚本 source 作为 `instanceKey`，重复运行时聚焦已有页面。命名窗口和显式多实例见[窗口实例](host-api.zh-CN.md#窗口实例)。页面运行于浏览器环境，不是 Node.js。普通浏览器不提供 `reaper.window.reaper`。

资源使用相对路径，入口采用延迟执行的普通脚本 `<script src="app.js" defer></script>`。使用打包器时，应输出适合本地 HTML 入口的文件，并在各平台 WebView 中验证资源、模块及网络请求。`reaper.window.open` 只接受本地 `.html` 或 `.htm` 文件，不能直接打开开发服务器网址。

模板的 `jsconfig.json` 开启 JavaScript 类型检查，不生成编译产物，配置含义见 [TypeScript checkJs 文档](https://www.typescriptlang.org/tsconfig/checkJs.html)。使用 TypeScript 时，包含全部三个 `.d.ts` 文件，并先将 `.ts` 编译为浏览器 JavaScript。SDK 声明的是全局对象，不需要 `import reaper`。

## EEL2 Backend

SDK 提供[最简 EEL2 后端](../examples/backends/eel-backend/README.zh-CN.md)和[双向轨道混音示例](../examples/backends/eel-mixer/README.zh-CN.md)。在 Action List 加载相应 `.eel` 脚本，无需 Lua 启动器或额外依赖。两个示例均使用 `ReaWeb_SetFrame` 刷新状态，音量范围为 `0..4`，推子写入包含执行确认、未确认请求限流和最终值保护。

v0.3.9.14 起，`ReaWeb_Open` 第四参数支持 EEL2 `0/1` 和 Lua `false/true`，省略行为不变。EEL2 原生字符串返回值使用第一个输出参数，例如 `ReaWeb_Receive(#message, id)`。直接调用和 `extension_api` 均可用，窗口关闭后应停止 `defer` 并通过 `atexit` 清理。完整接口、字符串限制和验证命令见 [EEL2 支持说明](eel-backend-compatibility.zh-CN.md)。

## 调用约定

网页通过 `window.reaper` 的运行时命名空间访问宿主。 已认证的外部 WebSocket 客户端可通过 `system.getCapabilities` 查询具名服务、事件和流。

```javascript
await reaper.lifecycle.ready;
const track = await reaper.audio.getSelectedTrackRef();
if (track) {
  const {name} = await reaper.audio.getTrackMix(track);
  console.log(name);
  await reaper.transaction.withUndo('Center pan', () =>
    reaper.audio.setTrackValueLatest(track, 'D_PAN', 0));
}
```

读取工程名和轨道数可调用 `reaper.host.service('runtime').invoke('getProjectSummary')`.

## 事件、界面和存储

```javascript
const stop = await reaper.events.on('track-selection-changed', state => {
  console.log('选中轨道数：', state.count);
});
// 界面组件卸载时：
await stop();
```

订阅会提供初始快照及合并后的变化，工程变化和原生轨道选择通知在每个主线程调度周期检查，轨道选择另保留 100 ms 兜底检查。它们用于刷新界面，不是编辑历史或采样时钟。异步回调需自行处理异常，组件卸载时取消订阅，整个页面关闭时会自动清理本页订阅。

停靠切换会保留页面状态。App 共用一个浏览器 profile，localStorage 和 IndexedDB 按 origin 隔离，cookie 遵循原生域和路径规则。来源持久化与存储规则见 [Web Runtime 约定](frontend.zh-CN.md)。保存 GUID 或工具设置，不要保存对象句柄，下次打开时针对正确的工程重新解析引用。浏览器存储和网络行为由各平台 WebView 决定。ReaWebAPI 不提供 Node.js 文件系统或 shell 接口。

## 错误与调试

桥接失败会拒绝 Promise，错误对象包含 `code` 和可选 `details`。REAPER 原生返回的 `false`、`0`、`null` 保持其文档含义，不会自动转成异常。

```javascript
try {
  const track = await reaper.audio.getSelectedTrackRef();
  if (track) console.log(await reaper.audio.getTrackMix(track));
} catch (error) {
  console.error(error); // 桥接错误带有 code 和可选 details。
}
```

`await reaper.debug.getDiagnostics()` 可查看后端、加载阶段、队列计数和最近宿主错误。在 WebView 或检查器中，Windows/Linux 按 Ctrl+Shift+I，macOS 按 Option+Command+I 显示或隐藏 DevTools，也可调用 `reaper.debug.openDevTools()` 请求打开。Windows/macOS 页面右键菜单还可控制显示状态及 Embedded/Floating 模式。布局、偏好保存和平台限制见 [DevTools](devtools.zh-CN.md)。修改资源后关闭窗口，再重新打开页面。重载会建立新文档，旧句柄失效。

尚未开始的原生请求在 25 秒后过期，客户端另有 30 秒等待保护。原生执行开始后停止排队计时，JavaScript 无法中断已开始的对话框或渲染。不要自动重试超时写入，应先检查状态。限制及错误码见 [宿主接口](host-api.zh-CN.md#错误与限制)。

## 分发工具

多窗口回归使用 `python tests/multi_window_stress.py --reaper <可执行文件> --extension <扩展文件> --output <新目录> --counts 8,16,32 --seconds 30`。窗口总数包含一个官方 Demo。测试使用独立配置，覆盖空工程及 50 轨、80 个合成 JSFX 负载，运行独立 Lua 循环、有界持续通信、帧刷新和乱序关闭重开。`cases.tsv`、`windows.tsv` 及诊断快照记录延迟与各窗口通信进度。合成 DSP 负载不等同于第三方采样器工程。

`latency.tsv` 分别记录稳定阶段的消息往返时间、Lua 轮询间隔，以及收到页面回显时的帧内容年龄。帧内容年龄包含回传耗时。`--no-frames` 关闭通信测试窗口的帧订阅以便对照，官方 Demo 保持不变。新旧版本对比应使用相同的 REAPER 可执行文件和负载。

Linux 几何失效逻辑由 CTest 中的 `linux_geometry` 覆盖。在 X11 显示环境中构建并运行 `linux_geometry_live`，可验证原生移动缩放、隐藏显示及窗口重建。WSL 未提供 DRI3 时，仅 WebKit 辅助进程默认使用 `GDK_RENDERING=image`，已有设置保持不变。该设置选择 GTK 软件后备缓冲，不修改 WebKit 的加速设置。参见 [GTK 绘制模式](https://docs.gtk.org/gtk3/running#environment-variables)。

分发 Lua 启动器、HTML、编译后的 JavaScript、CSS 和所需资源，保持相对路径。将模板 Action 描述及 `index.html` 的 `<title>` 改为自己的工具名称。动态名称和显式覆盖见[窗口标题](runtime-api.zh-CN.md#窗口标题)。类型声明和编辑器配置只供开发使用。用户需要安装 ReaWebAPI 原生扩展，不需要 Python 或编译器。

注明最低 REAPER 和 ReaWebAPI 版本，通过能力查询检查依赖的方法，并在界面解释缺失依赖。发布前检查空工程、没有选择、Unicode 路径、对象删除、工程切换、停靠、反复开关窗口及各目标系统。完整 Demo 可辅助桥接诊断，模拟 ABI 测试通过不代表已验证所有原生 API 对真实工程的影响。

`ReaWebAPI-ReaPack-v<版本>.zip` 包含 `ReaWebAPI.ext`、存放原生文件的 `extension/` 和存放 Demo、API 手册的 `web/`，三者同级。SDK ZIP 提供开发资料。

描述文件自动列出所有 Demo 文件，安装到该包 Scripts 路径下的 `web/` 目录。`ReaWebAPI_Demo.lua` 注册到 Action List，其余资源保留相对路径。启动器通过 `@noindex` 随扩展包统一管理。

## 现代前端与宿主 I/O

Vite/TypeScript、loopback 开发入口、原生本地资源 fetch与 Worker 示例见[前端资源约定](frontend.zh-CN.md)。统一文件读写、剪贴板和外链见[宿主参考](host-api.zh-CN.md#文件与桌面服务)。
