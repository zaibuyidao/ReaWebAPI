# ReaWebAPI

[Native Event 与 Host Service](docs/native-services.zh-CN.md) 提供按订阅启停的原生状态监测和 C/C++ 扩展注册，独立于 Lua 循环，与现有 Lua Backend 共存。

[English](README.md) | **简体中文**

Space 路由的初始值由普通 App 的 `app.json.dawKeyboard` 或 JSFX 的 `JSFX_WebUI.dawKeyboard` 决定，默认值分别为 `false` 和 `true`。两者均支持 [`reaper.window.setDawKeyboard()`](docs/runtime-api.zh-CN.md#space-路由) 动态覆盖，重载恢复 JSON 配置，不回写文件。

ReaWebAPI 是用于开发 HTML、CSS 和 JavaScript 工具的 REAPER 原生扩展，提供可停靠的 WebView 窗口，并通过 `reaper` 对象异步调用 REAPER API。

- [帧通道](docs/frame-timer-verification.zh-CN.md)支持原生播放状态采样和可替换的 Lua 内容，在 Windows、macOS、Linux 上独立于 Lua 轮询频率运行。小帧无需工作线程轮转，上一批编码事件投递期间，后续状态更新只保留最新值。
- [JSFX Web UI](docs/jsfx-webui.zh-CN.md) 托管关联配置的轨道、主轨、Take、输入、监听及嵌套容器 JSFX，通过 `params` 提供实例绑定的参数同步、宿主参数元数据、`enum` 中的已加载实例枚举选项（不可用字段为 `null`）、归一化写入和参数组撤销。`editor` 提供持久保存的共享内容尺寸。可选 EEL SDK 提供 DSP 流、复杂状态、命令和二进制资源。[原生通道](docs/jsfx-native-channel.zh-CN.md) 使用固定大小的暂存程序，降低初始化时的 EEL 编译开销。Windows、macOS 和 Linux 的初始能力与状态读取无需额外等待一次宿主调度。SDK 管理的 DSP 流在 Session 更换或传输恢复时保留原有订阅。
- 730 项 REAPER 7.80 标准 API 绑定及 TypeScript 类型声明。
- Mirror-aware Batch Builder，支持 456 项已审核 API（含播放查询和时间换算）、延迟引用、多返回值解构和结果类型推导。
- 支持 Lua 或 EEL2 后端 + WebView UI，通过 `ReaWeb_Send`、`ReaWeb_Receive`、`reaper.host.send` 和 `message` 事件通信，多窗口公平轮转投递，通信调度预算独立于工程采样。
- 可选 [External Client](docs/external-clients.zh-CN.md)，通过带认证的本机 WebSocket 复用 Native API、Batch、Service、Event 和二进制 Stream。服务默认关闭，可在 ReaWebAPI Preferences 中启用，默认地址为 `ws://127.0.0.1:9123`。
- 可选[透明浮动窗口](docs/host-api.zh-CN.md#透明窗口)，支持桌面 Alpha、原生后端能力检测及原 Dock 偏好保留。
- [无边框浮动窗口](docs/host-api.zh-CN.md#无边框窗口)支持动态切换标题栏、原生圆角和拖动区，覆盖 Windows、macOS 与 Linux X11/XWayland。
- 15 个 Runtime 命名空间，涵盖窗口、事件、文件、原生对话框、拖放、音频、Undo 和应用服务。
- 稳定的 `reaweb://<appId>` 来源，原生加载本地资源、模块和 Worker，持久化 App 存储，不创建每 App TCP listener。
- [浏览器文件与媒体](docs/browser-media.md)，支持可选的 Blob 导出，以及绑定文档生命周期的只读媒体 URL、有界流式读取和跳转。
- 停靠和浮动 WebView 缩放时保留浏览器画面，原生背景跟随页面 HTML/body 的不透明背景色。Windows、macOS 和 Linux 对[常见计算 sRGB 颜色跳过 Canvas 初始化](docs/frontend.zh-CN.md#普通-web-app)。默认 Windows 窗口取消停靠后的标题栏和边框保留系统样式。
- Windows 和 macOS 在页面首帧准备完成后显示新窗口，现有 App 无需修改启动代码。
- Windows、macOS 和 Linux 隐藏 JSFX 原生控件时跳过对已隐藏控件的多余几何查询，并在每次扫描内复用电平表句柄。
- Windows、macOS 和 Linux 在宿主空闲时准备空白 WebView 容器池，普通 App 与 JSFX 共用。[启动诊断](docs/devtools.zh-CN.md#启动计时)分别记录容器池使用情况、JSFX 初始化与缓存复激活、宿主观察计数、背景同步和原生显示。
- Windows/Linux [DevTools](docs/devtools.zh-CN.md) 支持可调宽度的右侧面板、浮动模式和布局偏好保存，Ctrl+Shift+I 切换显示。Windows 提供无边框嵌入面板、原生浮动窗口及页面右键菜单控制。
- macOS [Web Inspector](docs/devtools.zh-CN.md#macos) 支持右侧嵌入面板、原生浮动窗口和布局偏好保存，Option+Command+I 与页面右键菜单控制同一个检查器会话。
- WebView、标题栏与 Docker 页签右键菜单统一提供停靠、刷新、DevTools、打开 App 文件夹和关闭操作。HTML favicon 或 `reaper.window.setIcon(path)` 设置的 PNG、ICO、SVG 窗口图标在停靠切换和重载后保留，通过 `reaper.window.setIconVisible(boolean)` 即时控制显示，包括默认启动图标。Windows、macOS 和 Linux 的浮动 Docker 标题栏跟随当前 WebView 的图标设置。
- JavaScript、TypeScript 模板，Runtime Studio 示例和中英文文档。

Windows WebView 遵循 **Hide pointer while typing** 设置，输入时隐藏光标，在 REAPER 原生窗口上移动鼠标后恢复。参见[光标行为](docs/frontend.zh-CN.md#文本输入时的鼠标光标)。

Windows FX 链中的 JSFX Web UI 请使用 v0.3.8.28 或更高版本，避免切换窗口焦点时 REAPER 无响应。

## 下载与安装

从 [Releases](https://github.com/zaibuyidao/ReaWebAPI/releases) 下载与 **REAPER 进程架构**一致的安装包。

| 平台 | 架构 | 运行环境 |
| --- | --- | --- |
| Windows | x64 | Windows 10/11、WebView2 Evergreen、VC++ x64 运行库 |
| macOS | ARM64、Intel x64 | macOS 14+ |
| Linux | x64、ARM64 | Ubuntu 24.04 或兼容系统、WebKitGTK 4.1、X11/XWayland |

支持 REAPER 6.68+，完整 API 目录对应 REAPER 7.80+。

| 下载项 | 内容 |
| --- | --- |
| 平台 ZIP | 扩展、SDK、示例和文档 |
| SDK ZIP | TypeScript 类型声明、开发模板和 API 参考 |
| ReaPack ZIP | 多平台二进制文件、完整 Demo 和 ReaPack 仓库索引元数据 |

在 REAPER 中通过 **Options → Show REAPER resource path in explorer/finder** 打开资源目录。退出 REAPER，将平台 ZIP 解压到该目录，再启动 REAPER。

手动安装时，将原生扩展放入 `UserPlugins/`。Linux 的同架构 `reawebapi-webview-<arch>` 辅助程序也放在该目录，并赋予执行权限。

Linux 复用未变化的 X11 几何信息，通过宿主事件循环读取 WebKit 入站消息。WSL 未提供 DRI3 时，辅助进程使用 GTK 图像后备缓冲以减少绘制停顿。显式设置的 `GDK_RENDERING` 优先。

在 Action List 运行 `Scripts/ReaWebAPI/Example/ReaWebAPI_Demo.lua` 即可打开示例。

**Preferences > Plug-ins > ReaWebAPI** 提供 **Use sRGB for WebView rendering on Windows** 复选框，默认不勾选。Windows 上勾选后重启 REAPER，将为全部 ReaWebAPI WebView 请求固定 sRGB 显示配置。**Restore defaults** 取消勾选，通过 Apply/OK 保存修改。macOS 和 Linux 上该复选框禁用。参见[色彩配置](docs/frontend.zh-CN.md#webview-色彩配置)。

**Preloaded WebView containers** 设置待命的空闲容器数量，范围为 0..32，默认 2。Apply/OK 后立即生效，0 关闭预加载，Restore defaults 恢复为 2。容器池仅加载空白页，在宿主空闲时补充。App 资源、脚本和 JSFX 状态仍在打开时加载，使用过的容器不会放回池中。没有就绪容器时按原方式创建。预加载会增加内存占用，不能省去页面初始化和首帧准备。

## 开发

同页 **Diagnostics** 提供独立的 App 与 Runtime 调试开关、精确 Target App ID 和日志目录入口，设置应用后即时生效。参见[运行时诊断](docs/diagnostics.zh-CN.md)。

Lua 通过 `reaper.ReaWeb_Open(path, instanceKey)` 打开应用。传入 `debug.getinfo(1, "S").source` 作为 `instanceKey` 可复用该启动脚本的窗口，省略 key 则每次创建新窗口。JavaScript 等待 `reaper.lifecycle.ready` 后，可调用 REAPER Mirror 和 `reaper.window`、`reaper.events` 等 Runtime 接口。API 以 Promise 返回结果，参数和返回值顺序遵循 REAPER 的 Lua 签名。

基础模板位于 `SDK/starter/`，Vite 与 TypeScript 模板位于 `SDK/modern/`。运行 `SDK/runtime-demo/Open.lua` 可打开 Runtime Studio。

使用 HTML `<title>` 或 `document.title` 设置窗口名称，关闭后恢复停靠的 Docker 标签和单标签浮动 Docker 标题栏同样适用。`reaper.window.setTitle(title)` 可显式覆盖。`instanceKey` 仅控制实例复用。详见[窗口标题](docs/runtime-api.zh-CN.md#窗口标题)。

外部 HTTP、HTTPS、mailto 导航交给系统默认程序打开，App 页面保持不变。其他本地 HTML 使用 `reaper.window.open(path)` 打开。详见[导航行为](docs/frontend.zh-CN.md#导航行为)。

[SDK](runtime/README.zh-CN.md) · [开发指南](docs/development.zh-CN.md) · [REAPER API](docs/api-reference.md) · [Runtime API](docs/runtime-api.zh-CN.md) · [Lua 宿主 API](docs/host-api.zh-CN.md) · [Web 运行环境](docs/frontend.zh-CN.md) · [DevTools](docs/devtools.zh-CN.md)

所有 App 共享 `ReaWebAPI/WebViewData/`，App 身份、私有数据和窗口状态保存在 `ReaWebAPI/Apps/<appId>/`。App ID、origin 隔离与原生 Cookie 规则见[存储约定](docs/frontend.zh-CN.md#资源来源与存储)。

## 构建

需要 CMake 3.24+、C++17 工具链和 Python 3.10+，测试使用 Node.js。Linux 开发依赖为 `pkg-config`、`libwebkit2gtk-4.1-dev` 和 `libx11-dev`。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix stage
```

Windows 使用 MSVC x64，并添加 `-A x64`。macOS 使用 `-DCMAKE_OSX_ARCHITECTURES=arm64` 或 `x86_64` 指定架构。

扩展版本统一由 `CMakeLists.txt` 中的 `project(ReaWebAPI VERSION …)` 定义，支持第四段修订号。推送到默认分支后，工作流会构建全部平台，通过检查后发布 `v<版本号>`。当前更新内容维护在 `docs/release-notes.md`，已发布版本不会被覆盖。

[源码结构](docs/source-layout.md) · [API 维护](api/README.zh-CN.md) · [版本更新](docs/release-notes.md) · [第三方声明](THIRD_PARTY.md)

采用 [LGPL-3.0-or-later](LICENSE.md) 许可。

纯 JavaScript 应用使用 REAPER Mirror 和 Runtime API。需要由 Lua 管理工程逻辑时，参见 [Lua 后端 + WebView 示例](web/lua-backend/README.md)。平台安装包将其放在 `Scripts/ReaWebAPI/Example/lua-backend`，SDK 提供 `lua-backend/`。

EEL2 后端使用同一消息桥。SDK 提供[最简示例](web/eel-backend/README.zh-CN.md)和[双向轨道混音示例](web/eel-mixer/README.zh-CN.md)，分别安装于 `eel-backend/` 和 `eel-mixer/`。两个示例均使用 `ReaWeb_SetFrame` 刷新状态，音量范围为 `0..4`，推子写入包含执行确认、未确认请求限流和最终值保护。

Native Stream 在多路分析间复用轨道采样，支持 `aggregate: true`，用于 Folder 和 Receive Bus 的 pre-FX 源内容同步聚合分析。见 [接口契约](docs/native-streams.zh-CN.md) 和 [Demo](web/native-stream/README.md)。

路由中的父轨、总线或上游轨道不含媒体时，聚合 RMS/LUFS 历史仍会持续累计。

内置 Meter 提供 Sample/True Peak、独立的 Sample/True Peak 削波计数、每通道及全局峰值最大值、RMS-M/I、LUFS-M/S/I 和 LRA 上下界，支持复位及 Mono 标定，支持 1–32 源通道、精确整数削波计数和可选历史累计策略，分析独立于发布频率。

Windows、macOS 和 Linux 支持原生[隐藏启动](docs/host-api.zh-CN.md#隐藏启动)与后台预加载，参见[示例](runtime/hidden-start/README.md)。
