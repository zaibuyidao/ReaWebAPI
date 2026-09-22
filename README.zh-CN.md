# ReaWebAPI

[English](README.md) | **简体中文**

ReaWebAPI 是用于开发 HTML、CSS 和 JavaScript 工具的 REAPER 原生扩展，提供可停靠的 WebView 窗口，并通过 `reaper` 对象异步调用 REAPER API。

- 730 项 REAPER 7.80 标准 API 绑定及 TypeScript 类型声明。
- Mirror-aware Batch Builder，支持 173 项已审核 API、延迟引用、多返回值解构和结果类型推导。
- 支持 Lua 后端 + WebView UI，通过 `ReaWeb_Send`、`ReaWeb_Receive`、`reaper.host.send` 和 `message` 事件通信。
- 14 个 Runtime 命名空间，涵盖窗口、事件、文件、原生对话框、拖放、音频、Undo 和应用服务。
- 原生 WebView，支持模块、本地资源、Worker 和应用存储持久化。
- Windows/Linux [DevTools](docs/devtools.zh-CN.md) 支持可调宽度的右侧面板、浮动模式和布局偏好保存，Ctrl+Shift+I 切换显示。Windows 提供无边框嵌入面板、原生浮动窗口及页面右键菜单控制。
- macOS [Web Inspector](docs/devtools.zh-CN.md#macos) 支持右侧嵌入面板、原生浮动窗口和布局偏好保存，Option+Command+I 与页面右键菜单控制同一个检查器会话。
- WebView 右键菜单提供 Dock/Undock 操作。HTML favicon 或 `reaper.window.setIcon(path)` 设置的 PNG、ICO、SVG 窗口图标在停靠切换和重载后保留，通过 `reaper.window.setIconVisible(boolean)` 控制显示。
- JavaScript、TypeScript 模板，Runtime Studio 示例和中英文文档。

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

在 Action List 运行 `Scripts/ReaWebAPI/Example/ReaWebAPI_Demo.lua` 即可打开示例。

## 开发

Lua 通过 `reaper.ReaWeb_Open(path)` 打开应用。JavaScript 等待 `reaper.lifecycle.ready` 后，可调用 REAPER Mirror 和 `reaper.window`、`reaper.events` 等 Runtime 接口。API 以 Promise 返回结果，参数和返回值顺序遵循 REAPER 的 Lua 签名。

基础模板位于 `SDK/starter/`，Vite 与 TypeScript 模板位于 `SDK/modern/`。运行 `SDK/runtime-demo/Open.lua` 可打开 Runtime Studio。

[SDK](runtime/README.zh-CN.md) · [开发指南](docs/development.zh-CN.md) · [REAPER API](docs/api-reference.md) · [Runtime API](docs/runtime-api.zh-CN.md) · [Lua 宿主 API](docs/host-api.zh-CN.md) · [Web 运行环境](docs/frontend.zh-CN.md) · [DevTools](docs/devtools.zh-CN.md)

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
