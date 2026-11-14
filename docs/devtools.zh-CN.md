# DevTools

[English](devtools.md) | **简体中文**

在 WebView 或其受管 DevTools 窗口中，Windows/Linux 按 **Ctrl+Shift+I**，macOS 按 **Option+Command+I** 显示或隐藏 DevTools。长按不会重复切换，隐藏后焦点返回页面。`reaper.debug.openDevTools()` 和 `ReaWeb_DevTools(id)` 请求打开或显示 DevTools，重复调用不会隐藏检查器。

| 平台 | 显示方式 | 模式切换 |
| --- | --- | --- |
| Linux / WebKitGTK | 默认嵌入右侧，提供可拖动分隔条 | 页面或 Docker 页签右键菜单的 **Float DevTools** / **Embed DevTools**，也可使用面板工具栏 |
| Windows / WebView2 | 默认嵌入右侧，提供可拖动分隔条 | 页面右键菜单的 **Float DevTools** / **Embed DevTools** |
| macOS / WKWebView | 默认嵌入右侧，提供可拖动分隔条 | 页面右键菜单的 **Float DevTools** / **Embed DevTools** |

页面、标题栏与 Docker 页签菜单统一提供停靠、遵循生命周期的刷新、DevTools、打开 App 文件夹和关闭操作，并勾选当前停靠状态。浏览器默认菜单项保持隐藏。各平台默认使用英文菜单。两个菜单的可见性和模式状态均来自同一个检查器控制器。

## Linux

面板默认占可用宽度的 40%，拖动分隔条可调整至 20%–80%。调整主窗口大小时保留比例。**Float DevTools** / **Dock right** 在容器间移动同一个检查器视图，保留 Console / Inspector 会话，不重载页面。

Ctrl+Shift+I 在检查器内同样有效。快捷键、工具栏 **Hide DevTools** 和浮动窗口关闭按钮只隐藏面板并保留会话。检查器自带的关闭控件可能结束会话，再次打开时检查器状态可能重置。

浮动窗口为非模态窗口，通过 X11 关联当前 REAPER 父窗口，层级受窗口管理器控制。需要 X11 或 XWayland。

## Windows

**Embedded** 以 WebView2 检查器内容填满右侧面板，隐藏标题栏、窗口控制按钮及边框，不保留装饰占位或可拖动标题栏。面板默认占可用宽度的 40%，拖动 1px 分割线可调整至 20%–80%，调整窗口大小时保留比例。分割线使用系统窗口边框颜色。**Floating** 恢复检查器原生标题栏、窗口控制按钮和边框，不再叠加宿主窗口。模式切换复用同一个检查器窗口，保留 Console / Inspector 状态，不重载页面。

在 WebView 页面上右键，DevTools 操作与 **Dock <App> in Docker**（停靠时勾选） 位于同一菜单。隐藏时显示 **Open DevTools**，显示时提供 **Hide DevTools**。嵌入模式提供 **Float DevTools**，浮动模式提供 **Embed DevTools**。隐藏或正在打开时禁用模式切换。检查器填满容器，不附加宿主工具栏，Demo 也不再单独提供 DevTools 按钮。

右键菜单、快捷键和 API 共用状态管理。Ctrl+Shift+I 和 **Hide DevTools** 只隐藏检查器并保留会话。浮动窗口的原生关闭按钮会结束检查器会话。浮动窗口跟随当前 REAPER 根窗口，其他 REAPER 窗口激活时不抢焦点地提升层级，不设置全局置顶。焦点移至 DevTools 不会暂停或重载页面。Windows 嵌入式 DevTools 支持多个停靠 WebView 之间的切换，包括 REAPER 与 WebView2 使用不同 DPI 上下文的情况。

WebView2 没有公开的嵌入式检查器控制器。ReaWebAPI 通过 [`OpenDevToolsWindow`](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2#opendevtoolswindow) 创建并识别原生检查器窗口，在 Embedded 模式下依据实际渲染起点和客户区边界裁去 Chromium 自绘标题栏及边框。Per-Monitor V1/V2 宿主通过兼容的 DPI 容器托管，不修改 REAPER 或 Chromium 的进程 DPI 模式。内容边界不可用、不支持的 DPI 组合或托管失败时保留原生浮动窗口，并禁用 **Embed DevTools**，`devtools.fallbackReason` 返回降级原因。窗口识别或检查器内按键处理失败时，`devtools.lastError` 返回对应限制。无法识别的窗口需使用自身关闭按钮。此集成依赖 WebView2 原生窗口实现。

## macOS

**Embedded** 将原生检查器内容视图放在页面右侧，不显示独立窗口标题栏、窗口按钮、外层边框，也不预留装饰空间。**Floating** 将同一视图放回 WebKit 自身的检查器窗口，不额外创建 ReaWebAPI 窗口或工具栏。

页面右键菜单根据当前状态显示 **Open DevTools** / **Hide DevTools** 和 **Float DevTools** / **Embed DevTools**。在页面或检查器中按 **Option+Command+I** 切换显示，隐藏时禁用模式切换。快捷键、菜单、`reaper.debug.openDevTools()` 和 `ReaWeb_DevTools(id)` 共用控制器。隐藏和切换模式保留检查器视图及连接，包括 Console 日志和当前选中的标签。原生关闭控件会结束会话。

面板默认占可用宽度的 40%，拖动检查器分隔线可调整至 20%–80% 并保存比例。调整窗口大小时保留比例，窄窗口同样支持嵌入，不会因宿主尺寸禁用 **Embed DevTools** 或强制切换为 Floating。

公开的 `WKWebView.inspectable` 保持启用。公开 API 没有对应控制器，程序化控制与显示使用经过运行时能力检查的 WebKit 私有接口。AppKit 负责内容视图布局，检查器的停靠控件复用现有模式和宽度偏好。兼容性取决于系统 WebKit 版本，嵌入控制不可用时保留原生浮动窗口，`embeddedSupported` 返回 `false`。原生控制不可用时禁用菜单操作，`nativeToggleSupported` 返回 `false`，`openDevTools()` 以 `DEVTOOLS_UNAVAILABLE` 错误拒绝。仍可通过 Safari 的开发菜单手动检查页面。`fallbackReason` 和 `lastError` 提供能力限制或打开失败的原因。

## 状态保存与诊断

`ReaWebAPI/Apps/<appId>/WindowState/*.json` 按页面及窗口槽位保存 `devtools.mode`（`embedded` / `floating`）和 `devtools.widthRatio`。重新打开工具或重启 REAPER 后恢复偏好。DevTools 初始关闭，不持久化调试会话。各平台恢复已保存模式。Windows/macOS 托管降级不会覆盖已保存偏好。

`(await reaper.debug.getDiagnostics()).devtools` 返回实际模式、宽度比例、嵌入支持情况及后端状态。macOS 的 `visible` 反映本地 WebKit 检查器状态，Windows 的可见状态仅反映 ReaWebAPI 已识别的原生窗口。

生产资源在 Console、Sources 和 Network 中使用 `reaweb://<appId>/...` URL。原生 Inspector 会话、快捷键及 Embedded/Floating 控制保持原有行为。

持久化日志在 REAPER Preferences 中独立配置，参见[运行时诊断](diagnostics.zh-CN.md)。

## 启动计时

macOS 和 Linux 的绘制前编辑器探针计入共享观察器的检查与准备阶段统计。Windows 专用的钩子和窗口遮蔽计数在这两个平台仍为零。

`controllerPrewarmed` 表示该窗口是否使用预备的空白容器。`idleWebViews` 提供共享池的目标数量 `target`、就绪数量 `ready`、未就绪数量 `pending`，以及当前平台实例累计的创建 `created`、取用 `taken` 和失败 `failed` 次数。取用容器不表示页面已加载或显示。Linux 从辅助进程异步接收这些计数。

`(await reaper.debug.getDiagnostics()).startup` 为每个窗口保留一条有界的原生计时记录。`kind` 为 `create`、`navigation` 或 `cachedReopen`。`windowId` 和 `documentGeneration` 标识文档，JSFX 另带 `sessionId` 和 `activation`。各阶段以 App 打开请求、观察到的 JSFX 发现、后续导航或缓存复激活为起点，单位为毫秒。FX 发现计时从现有扫描检测到编辑器后开始，不包含此前的轮询间隔。

Windows、macOS 和 Linux 共用以下 JSFX 原生计时点。每个点记录当前启动记录内的首次观察值，不可跨导航或缓存激活配对。

| 操作 | 起点 | 终点 |
| --- | --- | --- |
| 能力处理 | `connectMs` | `capabilitiesReadyMs` |
| 参数快照 | `parametersStartMs` | `parametersReadyMs` |
| 编辑器尺寸查找 | `editorSizeStartMs` | `editorSizeReadyMs` |

Windows、macOS 和 Linux 的 JSFX 缓存复激活另记录 `bindingReadyMs`、`associationReadyMs`、`hostPreparedMs`、`sizeRestoredMs`、`cacheMatchedMs`、`reparentReadyMs`、`hostAttachedMs` 和 `stateNotifiedMs`，分别标识绑定校验、关联查找、原生准备、尺寸恢复、缓存匹配、父窗口切换、挂接和可见状态分发完成。`stateNotifiedMs` 表示通知分发，不代表浏览器已接收。

`jsfxObserver` 提供发现器生命周期内固定数量的累计 `{ms, calls}` 计数，所有 JSFX 窗口共用。`notification`、`hook`、`scan` 分别记录宿主通知、Windows 绘制前钩子与周期发现。嵌套阶段包含 `inspect`、`identifyControls`、`resolveOwnership`、`hitTest`、`match`、`prepareEditor`、`createHost`、`conceal`、`restoreSize`、`cloak`、`hideControls`。`hook`、`prepareFloating` 和 `cloak` 仅在 Windows 记录，分别对应绘制前处理、浮动窗口准备和 DWM 窗口遮蔽，在 macOS 和 Linux 上保持为零。隐藏子阶段仅描述初次编辑器准备，不统计之后的每次隐藏。应比较同一区间的计数差值，不能把嵌套阶段与父阶段相加，也不能将累计计数当成单窗口启动时间。

可用阶段包括环境等待、Controller 创建、导航完成、Runtime 握手、首帧准备和原生揭示。Windows 的 `environmentStartup.initializationMs` 是完整的环境初始化尝试耗时。`environmentReadyMs` 减去 `environmentWaitStartMs` 是本窗口的剩余等待，包含原生宿主窗口创建。`mode` 区分环境已就绪与初始化中。导航与缓存复开不记录未重复执行的阶段。Linux 的 Controller 就绪与加载完成是从 WebKit 辅助进程收到的观察值，不提供独立的不透明页面快照确认。

`revealCompleteMs` 表示现有原生揭示路径完成，不代表显示器物理上屏或业务数据就绪。JSFX 的 `presentationMs` 含义保持不变。重叠阶段不能累加，前端与原生时钟不能直接相减。

HTML、脚本和样式加载可使用浏览器 Navigation/Resource Timing。JSFX 在 `performance.getEntriesByType('measure')` 中提供 `reaweb.jsfx.` 前缀的有界记录，涵盖能力就绪、订阅、编辑器尺寸、参数、连接、未完成尺寸请求等待与 present 确认。每阶段仅保留最新一条，`detail` 带实例与激活标识。`present` 包含现有尺寸等待，不代表物理显示延迟。Runtime Studio 以 `reaweb.demo.appInfo` 记录独立元数据查询。App 自有首屏数据和布局可使用单独的 User Timing 记录。计时不增加显示前置条件。

共享 JSFX SDK 还提供 `reaweb.jsfx.` 前缀的 `lifecycle`、`subscribe.<事件>`、`initialInvoke.<方法>` 和 `initialSessionRefresh`。订阅区间重叠，不能累加。初始化调用包含等待回复，不仅是原生处理耗时。`initialSessionRefresh` 记录初始 `STALE_SESSION` 后成功完成的能力刷新。

三个平台的非透明窗口均以 `reaweb.background.<阶段>` 记录 `rootStyle`、`bodyStyle`、`parse` 及 Canvas 回退（`context`、`clear`、`fill`、`read`）的首次调用。缺失阶段表示未执行，不是测得耗时为零。参见[背景转换](frontend.zh-CN.md#普通-web-app)。

在托管页面控制台查看原生计时与浏览器记录：

```js
const { startup } = await reaper.debug.getDiagnostics();
console.log(startup);
console.table(performance.getEntriesByType('measure')
  .filter(({ name }) => /^(reaweb\.jsfx\.|reaweb\.background\.)/.test(name))
  .map(({ name, startTime, duration }) => ({ name, startTime, duration })));
```
