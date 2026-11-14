# Web Runtime v1 与前端资源

[English](frontend.md) | **简体中文** · [宿主 API](host-api.zh-CN.md)

ReaWebAPI 的 **Web Runtime v1** 定义浏览器能力、资源加载和存储约定。Web 能力约定版本独立于扩展版本。JavaScript 通过 Promise 调用 730 项 REAPER 7.80 标准 API。

## WebView 色彩配置

**REAPER Preferences > Plug-ins > ReaWebAPI** 打开 **ReaWebAPI settings** 页面。**Use sRGB for WebView rendering on Windows** 默认不勾选，保留 WebView 和系统默认色彩管理。Windows 上勾选后请求固定 sRGB 显示配置，以减少 UI 颜色与 REAPER、ReaImGui 的差异。**Restore defaults** 取消勾选，通过 Apply/OK 保存。Cancel 丢弃尚未应用的修改，包括恢复默认操作。

| 平台 | 后端 | sRGB 覆盖 |
| --- | --- | --- |
| Windows | WebView2 | 创建共享浏览器环境时传入 `--force-color-profile=srgb`，适用于 ReaWebAPI 的全部 App 和开发 WebView。 |
| macOS | WKWebView | 无等价公开接口，控件禁用，保留原生色彩管理。 |
| Linux | WebKitGTK 4.1 | 无等价公开接口，控件禁用，保留原生色彩管理。 |

Windows 上通过 Apply/OK 保存到 REAPER 资源目录中的 `ReaWebAPI.ini`，配置项为 `[ReaWebAPI] WebViewColorProfile=Default|sRGB`。修改后需重启 REAPER，刷新页面或重新打开 App 不会应用待生效的修改。继续使用现有共享浏览器数据目录。

Windows 使用 [WebView2 浏览器开关](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/webview-features-flags)，行为与可用性可能随运行时版本变化。宿主环境或注册表覆盖可能优先于该设置。该设置不会修改显示器 ICC 配置、HDR 配置或其他插件，也不能保证所有显示器的物理颜色完全一致。[WKWebView](https://developer.apple.com/documentation/webkit/wkwebview) 和 [WebKitGTK 设置](https://webkitgtk.org/reference/webkit2gtk/stable/class.Settings.html) 没有对应的单个 WebView 显示配置接口，本版未实现这两个后端的覆盖。

## 文本输入时的鼠标光标

Windows 上，ReaWebAPI 遵循 **Hide pointer while typing** 设置。在 WebView 输入时隐藏光标，包括鼠标停留于 REAPER Arrange View 的情况。在 REAPER 原生窗口上移动鼠标后，光标恢复显示并保留原生形状，键盘焦点不变。鼠标静止时保持隐藏。App 无需添加处理代码，也无需修改系统设置。

当原生窗口中的鼠标移动无法恢复输入隐藏状态时，Windows 后端向对应 WebView2 窗口补发非客户区移动通知。macOS WKWebView 和 Linux WebKitGTK 保留原生光标处理。更新扩展后需重启 REAPER。

## 普通 Web App

Windows 和 macOS 首次显示等待页面加载和原生页面快照，避免在初始 HTML 和样式加载期间暴露空白的原生窗口或 WebView。渐变、背景图片和最终 CSS 画布颜色保持不变。文档 `load` 事件之后的异步业务数据加载仍由 App 管理。初始化失败或 15 秒内未取得首帧时，待显示窗口关闭并报告错误。Linux 保留 v0.3.8.10 的启动流程，在原生窗口注册完成后显示容器，不等待页面快照。

停靠和浮动 WebView 的原生缩放背景跟随 `html` 的不透明 CSS 画布背景色，或传播到画布的 `body` 背景色。根节点和 body 属性变化、样式表加载、样式节点变化、视口缩放和系统深浅色切换会刷新该颜色。透明或半透明画布背景保留浏览器的白色默认底色，背景图片和渐变仍由浏览器绘制。App 无需添加缩放处理代码。

Windows、macOS 和 Linux 的非透明窗口共用此背景同步路径。计算样式中的 `rgb()`/`rgba()` 在 sRGB 通道为整数且透明度为 0 或 1 时直接解析，其他颜色通过 Canvas 转换，保留颜色转换与 8 位透明度取整。显式设置 `{transparent:true}` 的窗口不使用此路径。

支持普通 HTML/CSS/JavaScript 目录，无需打包或 npm：

```text
MyApp/
  reawebapi-my-app.lua
  index.html
  app.js
  ui.js
  style.css
  data/config.json
  images/
```

```html
<link rel="stylesheet" href="./style.css">
<script type="module" src="./app.js"></script>
```

```js
import { render } from './ui.js';
const config = await (await fetch('./data/config.json')).json();
localStorage.setItem('theme', 'dark');
await reaper.lifecycle.ready;
const track = await reaper.GetTrack(0, 0);
render(config, track);
```

[无需构建的检查 App](../examples/runtime-features/web-runtime/README.zh-CN.md) 演示模块、本地 fetch、存储、Canvas、文件对象、DOM 拖放和 Worker。在 REAPER 中运行其 `reawebapi-web-runtime.lua`。桥接自动注入顶层页面，无需导入 `reaper.js`。镜像调用继续使用 `await`。

## v1 保证范围与可选能力

下表定义支持约定，适用于项目支持且正常维护的系统 WebView；不代表保证每一项最新语法或浏览器碰巧暴露的所有功能。

| 能力 | v1 约定 | 说明 |
| --- | --- | --- |
| HTML5、CSS、ES6+、DOM/事件、Promise、async/await、JSON | 必需 | 由浏览器原生实现；较新的特性仍需检测 |
| ES module、静态/动态 import | 必需 | 相对 URL、正确 JS MIME，无需构建；裸包名导入需 import map 或构建 |
| App 内本地 fetch | 必需 | 读取 App 根目录内资源，支持 JSON/文本/ArrayBuffer，文件不存在返回 404 |
| setTimeout/setInterval/requestAnimationFrame | 必需 | 隐藏或最小化时遵守浏览器节流行为，不是音频时钟 |
| Canvas 2D | 必需 | 使用原生浏览器上下文 |
| localStorage | 必需 | 在共享 profile 内按 origin 隔离并持久化，仍受原生配额和错误约束 |
| File/Blob/FileReader/ArrayBuffer | 必需 | 浏览器文件对象；任意磁盘读写使用宿主文件 API |
| 标准 DOM Drag & Drop | 必需 | `dragover`/`drop`/`DataTransfer`；不等于提供 REAPER 到系统的原生拖出 API |
| 外部 HTTP/HTTPS fetch、WebSocket | 可选 | 受对端 CORS、证书、CSP、网络和浏览器策略限制，不提供代理或 CORS 绕过 |
| IndexedDB、普通 Worker、Module Worker | 可选 | 检测能力，处理配额及运行错误 |
| WebGL | 可选 | 取决于浏览器、GPU、驱动与桌面环境 |
| Worker/iframe 内 REAPER 桥接 | 不提供 | Worker 通过消息通知顶层页面调用 `reaper` |

不专门支持 Service Worker、PWA、Push、定位、摄像头、麦克风、WebRTC、支付、Bluetooth、USB。WebView 暴露某些构造器不等于 ReaWebAPI 承诺支持。运行时不包含 Node.js/npm/Electron API。

## App 级跨源隔离

在 App 根目录 `app.json` 中设置 `"webRuntime": { "crossOriginIsolation": "required" }`。默认值为 `"off"`。`webRuntime` 未知字段、未知值或错误类型返回 `APP_MANIFEST_INVALID`。首次打开 App 时读取配置，重载及同 App 后续窗口沿用该配置。修改配置后须关闭该 App 的**全部窗口**再重新打开。

隔离资源返回 `Cross-Origin-Opener-Policy: same-origin`、`Cross-Origin-Embedder-Policy: require-corp` 和 `Permissions-Policy: cross-origin-isolated=(self)`。Windows 保持 `reaweb://<appId>`。macOS/Linux 的 required App 使用经过认证的 `http://127.0.0.1:<port>` 来源，在 `Apps/<appId>/isolation-origin.json` 中按 App ID 持久绑定端口。已保存端口被占用时返回 `APP_ORIGIN_IN_USE`，不临时切换来源。普通 App 保持自定义协议来源。外部脚本、图片、字体和媒体须提供兼容的 CORS/CORP 响应。App 自有 CSP 继续生效。普通和 Blob Worker 可使用 `worker-src 'self' blob:`，支持相应指令的浏览器可使用 `script-src 'self' 'wasm-unsafe-eval'` 编译 WASM。

App ID、私有文件和窗口状态不变。浏览器存储按来源隔离，macOS/Linux 启用 required 后使用独立的 localStorage/IndexedDB，保留旧数据但不自动迁移。如需旧浏览器状态，应在切换前导出。恢复 off 模式即可回到旧来源。请保留 `app.json`，并将来源记录与 App 数据一起备份。Linux 需要支持隔离进程 SharedArrayBuffer 的 WebKitGTK，参见[独立运行库构建](linux-webkit-runtime.zh-CN.md)。required HTTP 页面通过现有流通道接收工程视频，不使用自定义协议帧传输。

文档创建时检查浏览器实际隔离状态。失败后停止加载，并向宿主报告 `CROSS_ORIGIN_ISOLATION_FAILED`。浏览器未提供隔离能力时报告 `CROSS_ORIGIN_ISOLATION_UNSUPPORTED`。宿主关闭失败窗口，错误可从 REAPER Console 和原生 Runtime Diagnostics 获取，不依赖页面完成握手。普通 App 不受影响。

```js
await reaper.lifecycle.ready;
const { webRuntime } = await reaper.system.getCapabilities();
console.log(webRuntime.crossOriginIsolation, webRuntime.browser);
```

`crossOriginIsolation` 分别报告请求模式 `requested`、后端能力 `supported`、当前文档状态 `active`、原因 `reason`、初始化阶段 `stage`、失败阶段 `failureStage` 和文档代次 `documentGeneration`。`null` 表示未确认，不代表成功。`browser` 报告安全上下文、实际隔离、SAB、共享 WASM 内存、普通 Worker、Blob Worker 和 WebGL2。Worker 探针实际通信并交换共享内存，子线程状态见 `browser.workers`。`browser.errors` 区分创建/发送、加载/执行、消息、共享内存、资源及超时错误。超时或未检测为 `null`，已确认失败为 `false`。

`getCapabilities()` 与 `debug.getDiagnostics()` 等待并复用当前文档的一次探针结果。隔离 App 还会在握手后自动开始检测。文档就绪等待、Worker 和资源请求各有三秒超时，结束时释放 Worker 和 Blob URL。探针完成前，原生诊断可以返回 `null`。HTTP 开发页面报告实际文档隔离状态，但不能使用本地保留的 Worker 探针资源。WebGL2 独立于通用 WASM 线程能力，PSP 等应用另有要求。

直接使用浏览器标准 `Worker`、`SharedArrayBuffer`、`Atomics` 和 `WebAssembly.Memory`，不新增线程命名空间或 Worker 桥接。App 负责终止闲置 Worker，浏览器在文档关闭或重载时释放 Dedicated Worker。不要在页面主线程执行阻塞的 `Atomics.wait()`。

[线程 WASM 示例](../examples/runtime-features/isolation/README.zh-CN.md) 覆盖普通与 Blob 各四个 Worker、WASM 原子计算、等待/唤醒、异常和存储。设置响应头不等于引擎已支持。

## 资源来源与存储

浏览器 File/Blob 播放、原生文件选择及可选下载导出模块见[浏览器媒体接入](browser-media.md)。

`reaper.window.open(path)` 接受本地 HTML 路径，其规范化父目录是 **App Root**。生产页面通常使用 `reaweb://<appId>/<entry>`，macOS/Linux 的 required App 使用上述持久绑定回环来源。两种路径提供相同的只读资源。支持相对 URL、ES modules、本地 fetch、Unicode/空格/#/% 文件名、MIME、HEAD 和单字节范围请求。URL 中的 `#` 和 `%` 必须编码，例如 `file%23%25.js`。拒绝目录列表、写入、目录遍历及指向根目录外的 symlink/junction。

在根目录 `app.json` 中定义稳定且全局唯一的 `id`：

```json
{ "id": "timefold", "name": "TimeFold", "version": "1.0.5" }
```

对应入口为 `reaweb://timefold/index.html`，`reaper.app.getId()` 返回 `timefold`。ID 仅允许小写 ASCII `a-z`、`0-9` 和 `-`。显示名称与目录名称不参与身份计算。复制模板创建新 App 时应更换 ID。

未定义 `id` 时，普通 WebUI 使用持久化随机 ID，格式为 `app-<32 位小写十六进制字符>`，保存在 `<App 根目录>/.reawebapi/identity.json`，内容为 `{"schema":1,"appId":"app-…"}`。该目录随 App 移动，不通过 Web 资源响应暴露，解析后的别名路径也受保护。标记创建与绑定检查串行执行。标记无法读取或损坏时返回 `APP_IDENTITY_INVALID`，无法创建或更新时返回 `APP_IDENTITY_UNAVAILABLE`，不回退到临时 ID。不修改 `app.json`。

移动目录并保留标记时身份不变。完整复制目录后，若标记中的 ID 已绑定另一个有效安装，副本自动获得新 ID。旧绑定失效时沿用既有恢复规则。文件系统状态不明、绑定损坏，或仍有窗口使用已失效的旧绑定时继续报错。删除标记后，下次打开会初始化新身份。

Lua source 与 `instanceKey` 保留路径解析和窗口复用职责，不再生成 App ID。JSFX WebUI 继续通过 `fx_ident` 派生回退 ID，包括存在 `app.json` 但未定义 `id` 的情况。两类 WebUI 均优先使用有效的显式 `app.json.id`。`app.json.entry` 声明 HTML 入口，不是 App ID。

**升级行为：**普通 WebUI 没有显式 ID 和身份标记时，首次打开生成新的随机 ID。不查询、回退或迁移旧 launcher 派生 ID。原有 localStorage、IndexedDB、Data 和 WindowState 保留，但不自动关联到新身份。显式 ID 应用不受影响。

两个有效安装不能绑定同一 ID，即使先前窗口已关闭。第二个目录返回 `APP_ID_CONFLICT`。旧根目录不存在，或记录的原入口 HTML 已缺失、不再是常规文件时，只要没有活动或初始化中窗口使用旧绑定，即可自动恢复绑定。根目录按平台路径规则比较。无法确认文件系统状态时安全失败，冲突诊断包含新旧根目录及身份记录路径。修改 ID 会创建新的 origin 和数据目录。

继续兼容没有 `entry` 的旧 Schema 2 记录。旧根目录不存在且未被使用时可自动恢复。旧根目录仍存在时，可通过其 `app.json.entry` 确定原入口。缺少可靠入口元数据时保留 `APP_ID_CONFLICT`，不根据其他 HTML 文件或新请求猜测原入口。成功正常打开后补齐实际入口。恢复绑定保留 App ID、origin、私有数据、窗口状态文件和共享浏览器 profile。

所有 App 继续共享现有浏览器 profile。localStorage 和 IndexedDB 按 App 的实际来源隔离，重启后保留。Cookie 由浏览器原生规则决定，自定义协议可能不支持 Cookie。App 持久状态应使用 localStorage 或 IndexedDB。Bridge 文档仍分别持有 handles 和 subscriptions。

```text
<REAPER resource>/ReaWebAPI/
  WebViewData/
  Apps/
    <appId>/
      origin.json
      Data/
      WindowState/
```

Windows 共享一个 WebView2 Environment。Linux 共享一个 WebKitGTK context 与辅助进程。macOS 在 `WebViewData/` 保留共享 WKWebsiteDataStore UUID，实际数据库由 WebKit 管理。`origin.json` schema 2 记录 App ID、根目录与虚拟 origin，不包含端口。新建或更新的绑定还保存 `entry`，值为规范化的相对 HTML/HTM 路径，解析符号链接和目录联接后仍须位于根目录内。同一身份路径上的旧 schema 1 记录可升级。旧哈希 App 目录和浏览器数据继续保留，HTTP origin 的存储不自动迁移到虚拟 origin。身份元数据损坏时返回 `APP_ORIGIN_INVALID`。

备份时应同时保留共享 profile 与相关 `Apps/<appId>/` 目录。浏览器配额与用户清理数据的规则仍然适用。开发页面保留显式 HTTP origin，不创建生产 origin 记录。External Client 的监听器、配置与认证完全独立。

资源处理器拒绝其他 App authority 和 origin，不开放宽松 CORS。App Root 限制资源加载，不限制可信原生 API 的文件权限。REAPER 调用仍通过原有主线程 bridge 执行。

能力与诊断提供：
`{ contract: 1, mode: 'app-virtual' | 'app-loopback' | 'dev-http', appId, origin, storageIsolation: 'origin', localResources }`。
生产 App 报告实际 `origin`，两种本地资源模式的 `localResources` 均为 true。

## 导航行为

Windows、macOS 和 Linux 的 WebView 均保留入口文档。被拦截的外部 `http://`、`https://`、`mailto:` 导航使用与 `reaper.system.openExternal(url)` 相同的校验和系统默认程序，支持普通链接及 `location.href`、`location.assign()` 等 JavaScript 重定向。

本地 `file:` URL、App 或开发入口同源的其他路径和查询参数导航继续被拦截，并在开发者控制台显示提示。其他本地 HTML 使用 `reaper.window.open(path)` 打开。仅修改 hash 的导航继续允许，查询参数变化仍采用原有入口文档身份判断。`window.open()` 和请求新窗口的链接继续不受支持。

```html
<a href="https://www.extremraym.com">打开网站</a>
```

```js
window.location.assign('https://www.extremraym.com'); // 由系统浏览器打开。
await reaper.window.open('other.html');
```

## TypeScript 与开发服务器

[现代模板](../examples/templates/modern/README.zh-CN.md) 提供可选的 Vite/TypeScript **构建工具**。运行 `npm ci`、`npm run dev`，然后在 REAPER 运行 `reawebapi-modern-dev.lua`，默认地址 `http://localhost:5173/`。`reaper.window.openDev` 只接受 127.0.0.1、localhost 或 [::1] 上带明确端口的 HTTP URL。开发服务器由开发者启动，扩展不内置 Vite、Node 或 npm。

开发入口按完整 URL 分配 App 身份，浏览器存储在共享 profile 内按 origin 隔离，因此同一 origin 下的不同入口共享存储。应保持 URL 和端口稳定。允许 hash 导航，禁止切换到其他文档，入口应避免重定向。HMR 使用浏览器原生 WebSocket，已用 Vite 验证。

运行 `npm run build`、`reawebapi-modern.lua` 验证生产模式，一起分发 `reawebapi-modern.lua` 和 `dist/`。现有模板输出 IIFE 与 Blob Worker，这只是一种打包选择，普通 ES module 目录同样可用。两种模式均使用原生 `fetch('./data.json')`。宿主文件 API 以本地 HTML 目录为基准，此例是 `dist/`；由 Lua 打开的开发页以 REAPER Scripts 目录为基准。浏览器 URL 和原生文件路径是两个不同命名空间。

页面 CSP 需允许实际脚本/样式、本地 fetch 的 `connect-src 'self'` 和对应 `worker-src`；现代模板的 Blob Worker 需要 `worker-src blob:`，开发还需 Vite HTTP/WebSocket 地址。统一剪贴板与外链操作使用宿主 API。

Chromium 开发者工具读取 `/.well-known/appspecific/com.chrome.devtools.json` 同样需要 `connect-src 'self'`。文件不存在时，应用资源服务返回空配置。需要配置自动工作区时，开发者可在 App 中提供该文件，服务会优先读取它。

## 平台后端

| 平台 | 后端 | 存储隔离 |
| --- | --- | --- |
| Windows x64 | WebView2 | 共享 profile，按 App origin 隔离 |
| Linux x64 / ARM64 | WebKitGTK 4.1、X11/XWayland | 共享 context 与辅助进程，按 App origin 隔离 |
| macOS ARM64 / Intel | WKWebView | 共享持久数据存储，按 App origin 隔离 |

WebView2 取决于已安装运行时，WKWebView 跟随系统更新，WebKitGTK 取决于发行版包。Linux 扩展旁必须放同版本 helper。可选能力会随引擎和图形环境变化。HTTPS 交给 WebView 处理；自动网络测试验证 HTTP CORS 与 WebSocket，不代表验证任意外部 TLS 服务。DOM 拖放测试使用合成事件，实体系统文件拖入和真实 REAPER 拖放仍需人工验收。

普通生产来源为 `reaweb://<appId>`。macOS/Linux 的 required App 使用上述持久绑定回环来源。macOS 开发 HTTP URL 建议使用 `localhost`，参见 [Apple 本地网络规则](https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking)。

本次 WSLg 环境的 WebKitGTK DMA-BUF 渲染路径会使可见页面的动画帧停住；以 `WEBKIT_DISABLE_DMABUF_RENDERER=1` 运行 helper 后，包括原生 requestAnimationFrame 与 WebGL 的完整浏览器检查通过。这是环境开关，扩展不会默认强制设置，也不替换 JS 实现。目标 Linux 桌面应验证默认渲染器；遇到同类问题时，在启动 REAPER 前设置该变量。GTK 视口尺寸已按外部 X11 父窗口的客户区同步分配。

主要实现：`src/web/web_resources.*` 负责资源服务，`src/runtime/runtime.*` 管理 App 来源和共享 profile 生命周期，三个平台文件负责原生 profile 与页面加载，`runtime/reaper.d.ts` 定义能力元数据。测试覆盖原生资源边界、全部 730 项镜像 ABI 映射及实际浏览器行为。

原生行为参考：[WebView2 本地内容](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/working-with-local-content)、[WebKitGTK 持久化 cookie](https://webkitgtk.org/reference/webkit2gtk/2.42.5/method.CookieManager.set_persistent_storage.html)。Linux 已明确开启原生 cookie 持久化。

隐藏启动沿用相同资源加载和 Runtime 通道，原生窗口保持不可见。就绪仍表示文档握手，与渲染和业务数据独立。参见[隐藏启动](host-api.zh-CN.md#隐藏启动)和[预加载示例](../examples/runtime-features/hidden-start/README.md)。

## 原生拖动区

以 `{frameless:true}` 创建窗口或调用 `window.setFrameless(true)` 后，在矩形标题区域添加 `data-reaweb-drag`，在自定义交互区域添加 `data-reaweb-no-drag`。按钮、链接、输入框、选择框、文本框、可编辑或可聚焦元素及可滚动后代自动排除。自定义控件、覆盖层及 Shadow DOM 宿主应显式排除。页面应保留调用 `reaper.window.close()` 的可见关闭控件。

```html
<header data-reaweb-drag>
  <span>My App</span>
  <button id="close">关闭</button>
  <div data-reaweb-no-drag>自定义控件</div>
</header>
<script>
  document.querySelector('#close').onclick = () => reaper.window.close();
</script>
```

Windows 使用 WebView2 原生非客户区。macOS 和 Linux 在按下前缓存经过裁剪的 DOM 矩形，直接消费原生鼠标事件，不通过异步 Runtime 调用开始拖动。交互期间应保持标题区布局稳定。布局快照采样间隔为 32 ms，拖动矩形和排除矩形各最多 512 个，超限时停用拖动。任意形状和动画命中区域不在此合同内。Docker 内停用拖动区，拖动区外的右键操作保留既有页面菜单行为。参见[示例](../examples/windows/frameless/README.md)。

## 透明背景

以 `{transparent:true}` 打开，并显式设置 `html, body { background:transparent; }`。宿主不改写 App CSS。可在透明页面留白内使用半透明面板、圆角和 CSS 阴影，无边框工具应保留可见关闭控件。视觉透明仍采用矩形鼠标命中区域。参见[透明 Demo](../examples/windows/transparent/README.md)及[原生要求](host-api.zh-CN.md#透明窗口)。
