# ReaWebAPI

[English](README.md) | **简体中文**

ReaWebAPI 是 REAPER 的原生扩展，可用 HTML、CSS 和 JavaScript 开发可停靠的工具窗口和 JSFX 编辑器。页面在 REAPER 内运行，通过异步接口 `window.reaper` 与宿主交互。

[API 手册](https://zaibuyidao.github.io/ReaWebAPI/reawebapihelp.html) · [开发指南](docs/development.zh-CN.md) · [版本下载](https://github.com/zaibuyidao/ReaWebAPI/releases)

## 核心能力

- **应用与窗口**　本地页面可以浮动显示或停靠在 REAPER 中。应用可调用系统文件对话框、读写文件，并在下次启动时保留设置。
- **工程控制**　读取走带状态，订阅轨道和 FX 变化，调整轨道控件或 FX 参数。通过显式编辑手势，可将多次修改合并为一步撤销。
- **音频与视频**　通过原生音频流绘制波形、分析频谱和测量响度。工程视频预览的可用性取决于宿主和平台后端。
- **JSFX 编辑器**　将网页界面与 JSFX 实例关联，即可读写该实例的参数。效果器接入可选的 EEL SDK 后，还可提供 DSP 数据、命令和二进制资源。

## 工作原理

ReaScript 启动脚本通过 `ReaWeb_Open` 打开本地 HTML 入口。扩展负责管理窗口，并向页面注入 JavaScript 运行时。`reaper.lifecycle.ready` 就绪后，页面可直接调用运行时 API。如果工程逻辑保留在 Lua 或 EEL2 中，脚本可通过消息与页面通信。对于 JSFX 编辑器，DSP 和参数状态仍由 REAPER 管理。

其他原生扩展可通过 C/C++ SDK 注册具名服务和二进制数据流。外部客户端可通过本机 WebSocket 访问服务、事件和数据流。该服务默认关闭，启用后需要身份验证。

## 安装

需要 REAPER 6.68 或更高版本。请选择与 **REAPER 进程架构**一致的安装包。

| 平台 | 架构 | 运行环境 |
| --- | --- | --- |
| Windows | x64 | Windows 10 或 11、WebView2 Evergreen、Visual C++ x64 运行库 |
| macOS | Apple Silicon（ARM64）、Intel（x86_64） | macOS 13 或更高版本，系统自带 WKWebView |
| Linux | x86_64、ARM64 | Ubuntu 24.04 或兼容系统、WebKitGTK 4.1、X11 或 XWayland |

- **通过 ReaPack 安装**　导入 [zaibuyidao 仓库索引](https://github.com/zaibuyidao/ReaScripts/raw/master/index.xml)，安装 `ReaWebAPI: Web Runtime for REAPER`，然后重启 REAPER。示例和离线 API 手册会注册到动作列表。
- **使用平台 ZIP**　在 REAPER 中通过 **Options → Show REAPER resource path in explorer/finder** 打开资源目录，然后退出 REAPER。将对应平台的 ZIP 完整解压到该目录后重启。Linux 下需保留随包辅助程序的执行权限。平台包包含扩展、示例、SDK 和离线手册。

## 运行第一个应用

1. 打开 REAPER **动作列表（Action List）**。通过 ReaPack 安装后，搜索 `ReaWebAPI_Demo`。使用平台 ZIP 安装时，从资源目录加载 `Scripts/ReaWebAPI/Example/ReaWebAPI_Demo.lua`。运行动作即可打开示例。
2. 开发自己的工具时，将平台包中的 `ReaWebAPI/SDK/` 复制到可写目录，或使用发布页提供的独立 SDK。在动作列表中加载并运行该副本中的 `starter/reawebapi-starter.lua`。在 REAPER 中选择一条轨道，即可在示例窗口看到轨道名称。
3. 编辑 `starter/index.html` 和 `starter/app.js` 后重新加载页面。JavaScript 模板无需构建。使用 Vite 和 TypeScript 开发时，请选用 `modern/` 模板。

离线查阅 API 时，可直接打开平台包或 SDK 包中的 `ReaWebAPI/docs/reawebapihelp.html`。通过 ReaPack 安装后，可在动作列表中运行 `ReaWebAPI - API Reference`。

## 开发文档

| 主题 | 指南 |
| --- | --- |
| 应用开发 | [SDK 与模板](runtime/README.zh-CN.md) · [运行时 API](docs/runtime-api.zh-CN.md) · [ReaScript 宿主 API](docs/host-api.zh-CN.md) · [WebView 资源与存储](docs/frontend.zh-CN.md) |
| 宿主数据与媒体 | [实时订阅](docs/realtime.zh-CN.md) · [原生数据流](docs/native-streams.zh-CN.md) · [工程视频](docs/video-preview.zh-CN.md) |
| 扩展集成 | [JSFX Web UI](docs/jsfx-webui.zh-CN.md) · [原生服务](docs/native-services.zh-CN.md) · [外部客户端](docs/external-clients.zh-CN.md) |
| 调试 | [WebView 开发者工具](docs/devtools.zh-CN.md) · [运行时诊断](docs/diagnostics.zh-CN.md) |

[完整文档](docs/README.zh-CN.md) · [示例目录](https://github.com/zaibuyidao/ReaWebAPI/tree/main/examples) · [源码结构](docs/source-layout.md)

## 从源码构建

需要 CMake 3.24+、C++17 工具链、Python 3.10+ 和 Node.js 22.12+。Windows 使用 MSVC x64。Linux 需安装[构建工作流](https://github.com/zaibuyidao/ReaWebAPI/blob/main/.github/workflows/build.yml)中列出的 WebKitGTK 4.1 和 X11 开发包。

```sh
npm ci --prefix examples/templates/modern
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix stage
```

## 许可证

[LGPL-3.0-or-later](LICENSE.md) · [第三方软件声明](THIRD_PARTY.md)
