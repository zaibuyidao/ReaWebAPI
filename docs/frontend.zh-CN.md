# Web Runtime v1 与前端资源

[English](frontend.md) | **简体中文** · [宿主 API](host-api.zh-CN.md)

ReaWebAPI 的 **Web Runtime v1** 定义浏览器能力、资源加载和存储约定。Web 能力约定版本独立于扩展版本。JavaScript 通过 Promise 调用 730 项 REAPER 7.80 标准 API。

## 普通 Web App

支持普通 HTML/CSS/JavaScript 目录，无需打包或 npm：

```text
MyApp/
  Open.lua
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

[无需构建的检查 App](../runtime/web-runtime/README.zh-CN.md) 演示模块、本地 fetch、存储、Canvas、文件对象、DOM 拖放和 Worker。在 REAPER 中运行其 `Open.lua`。桥接自动注入顶层页面，无需导入 `reaper.js`。镜像调用继续使用 `await`。

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

## 资源来源与存储

`reaper.window.open(path)` 仍然接收本地 HTML 路径。入口经规范化后的父目录就是 **App 根目录**。扩展用只读本地监听器将它映射至 `http://127.0.0.1:<port>/`，由 WebView 发起正常 HTTP 请求。支持相对资源、中文/空格/#/% 文件名、查询参数、MIME、HEAD 和字节范围请求。资源 URL 中的 `#`、`%` 需要编码，例如 `file%23%25.js`。不列出目录，不提供 HTTP 写入或桥接端点；解析后的路径及符号链接/junction 不得越出根目录。

每个根目录保留独立的 App 身份与来源地址。所有 App 和 Module 共用一个浏览器 profile，localStorage 和 IndexedDB 仍按 origin 隔离。cookie 遵循原生主机、域与路径规则，同一主机的不同端口共享 cookie。句柄和订阅始终属于各自页面。请持久化设置或 GUID，不要保存原生句柄。

运行数据目录结构如下：

```text
<REAPER 资源目录>/ReaWebAPI/
  WebViewData/
  Apps/
    <appId>/
      origin.json
      Data/
      WindowState/
```

Windows 的活动 App 共用一个 WebView2 Environment，Linux 共用一个 WebKitGTK context 和辅助进程，浏览器数据保存在 `WebViewData/`。macOS 在该目录保存共享 WKWebsiteDataStore UUID，实际数据库位置由 WebKit 管理。`origin.json` 记录各本地 App 的根目录与端口，`Data/` 和 `WindowState/` 保持 App 私有。开发入口使用配置的 origin，不创建 `origin.json`。

重开或重启保留来源与共享 profile。移动、重命名 App 目录会产生新身份，原目录中的更新保留身份。旧的 App 浏览器数据和全局窗口状态不迁移。

若保存的端口被占用，返回 `APP_ORIGIN_BUSY`，不会悄悄改端口导致存储不可见。关闭冲突进程后重开即可。来源记录损坏或不匹配时返回 `APP_ORIGIN_INVALID`。不要把删除来源记录当作常规修复；新端口意味着新来源。浏览器配额、用户清理数据等正常限制仍然适用。

备份 App 状态时，应同时保留共享 `WebViewData/` 和相关 `Apps/<appId>/` 目录。

仅监听回环地址，拒绝外来 Host/Origin 及跨来源 Fetch Metadata 请求，不设置宽松 CORS。最后一个 App 窗口关闭后停止服务，每个活动 App 使用两个有队列上限的资源线程；REAPER API 仍经原有桥接在主线程执行。App 根目录是资源加载边界，**不是原生 API 沙箱**：可信页面仍有已开放的文件和工程权限。不要在资源目录内放秘密文件或打开不可信 App。

`reaper.system.getCapabilities()`、`reaper.debug.getDiagnostics()` 都返回 `webRuntime`：
`{ contract: 1, mode: 'app-http' | 'dev-http', appId, origin, storageIsolation: 'origin', localResources }`。
内建本地资源模式的 `localResources` 为 true。

## TypeScript 与开发服务器

[现代模板](../runtime/modern/README.zh-CN.md) 提供可选的 Vite/TypeScript **构建工具**。运行 `npm ci`、`npm run dev`，然后在 REAPER 运行 `OpenDev.lua`，默认地址 `http://localhost:5173/`。`reaper.window.openDev` 只接受 127.0.0.1、localhost 或 [::1] 上带明确端口的 HTTP URL。开发服务器由开发者启动，扩展不内置 Vite、Node 或 npm。

开发入口按完整 URL 分配 App 身份，浏览器存储在共享 profile 内按 origin 隔离，因此同一 origin 下的不同入口共享存储。应保持 URL 和端口稳定。允许 hash 导航，禁止切换到其他文档，入口应避免重定向。HMR 使用浏览器原生 WebSocket，已用 Vite 验证。

运行 `npm run build`、`Open.lua` 验证生产模式，一起分发 `Open.lua` 和 `dist/`。现有模板输出 IIFE 与 Blob Worker，这只是一种打包选择，普通 ES module 目录同样可用。两种模式均使用原生 `fetch('./data.json')`。宿主文件 API 以本地 HTML 目录为基准，此例是 `dist/`；由 Lua 打开的开发页以 REAPER Scripts 目录为基准。浏览器 URL 和原生文件路径是两个不同命名空间。

页面 CSP 需允许实际脚本/样式、本地 fetch 的 `connect-src 'self'` 和对应 `worker-src`；现代模板的 Blob Worker 需要 `worker-src blob:`，开发还需 Vite HTTP/WebSocket 地址。统一剪贴板与外链操作使用宿主 API。

Chromium 开发者工具读取 `/.well-known/appspecific/com.chrome.devtools.json` 同样需要 `connect-src 'self'`。文件不存在时，应用资源服务返回空配置。需要配置自动工作区时，开发者可在 App 中提供该文件，服务会优先读取它。

## 平台后端

| 平台 | 后端 | 存储隔离 |
| --- | --- | --- |
| Windows x64 | WebView2 | 每个 App 使用独立用户数据目录 |
| Linux x64 / ARM64 | WebKitGTK 4.1、X11/XWayland | 每个 App 使用独立辅助进程和数据目录 |
| macOS ARM64 / Intel | WKWebView | 每个 App 使用独立持久数据存储 UUID |

WebView2 取决于已安装运行时，WKWebView 跟随系统更新，WebKitGTK 取决于发行版包。Linux 扩展旁必须放同版本 helper。可选能力会随引擎和图形环境变化。HTTPS 交给 WebView 处理；自动网络测试验证 HTTP CORS 与 WebSocket，不代表验证任意外部 TLS 服务。DOM 拖放测试使用合成事件，实体系统文件拖入和真实 REAPER 拖放仍需人工验收。

macOS 对外来源使用 `http://localhost:<port>/`，监听器仍只绑定 127.0.0.1。macOS 14+ ATS 限制 IP 字面量 HTTP，使用未限定的本机名称可避免修改 REAPER 的 Info.plist。macOS 开发 URL 也建议使用 `localhost`。见 [Apple 本地网络规则](https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking)。

本次 WSLg 环境的 WebKitGTK DMA-BUF 渲染路径会使可见页面的动画帧停住；以 `WEBKIT_DISABLE_DMABUF_RENDERER=1` 运行 helper 后，包括原生 requestAnimationFrame 与 WebGL 的完整浏览器检查通过。这是环境开关，扩展不会默认强制设置，也不替换 JS 实现。目标 Linux 桌面应验证默认渲染器；遇到同类问题时，在启动 REAPER 前设置该变量。GTK 视口尺寸已按外部 X11 父窗口的客户区同步分配。

主要实现：`src/web/web_resources.*` 负责资源服务，`src/runtime/runtime.*` 管理 App 来源和共享 profile 生命周期，三个平台文件负责原生 profile 与页面加载，`runtime/reaper.d.ts` 定义能力元数据。测试覆盖 HTTP 边界、全部 730 项镜像 ABI 映射及实际浏览器行为。

原生行为参考：[WebView2 本地内容](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/working-with-local-content)、[WebKitGTK 持久化 cookie](https://webkitgtk.org/reference/webkit2gtk/2.42.5/method.CookieManager.set_persistent_storage.html)。Linux 已明确开启原生 cookie 持久化。
