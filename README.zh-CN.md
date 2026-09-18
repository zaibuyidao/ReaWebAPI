# ReaWebAPI

[English](README.md) | **简体中文**

REAPER 6.60+ 原生扩展，在可停靠的 WebView 中运行本地 HTML/CSS/JavaScript。Lua 负责打开页面，JavaScript 通过注入的 `reaper` 对象异步调用原生 API。

## 安装

从 [Releases](https://github.com/zaibuyidao/ReaWebAPI/releases) 下载，架构须与 **REAPER 进程**一致。

| 平台 | 扩展文件 | 运行环境 |
| --- | --- | --- |
| Windows x64 | `reaper_reawebapi-x64.dll` | Windows 10/11、WebView2 Evergreen、VC++ x64 运行库 |
| macOS ARM64 | `reaper_reawebapi-arm64.dylib` | macOS 14+ |
| macOS Intel | `reaper_reawebapi-x86_64.dylib` | macOS 14+ |
| Linux x64 | `reaper_reawebapi-x86_64.so` | Ubuntu 24.04 或兼容系统、WebKitGTK 4.1、X11/XWayland |
| Linux ARM64 | `reaper_reawebapi-aarch64.so` | 同上 |

退出 REAPER，把扩展放入资源目录的 `UserPlugins/`，再重启。Linux 还需将同架构的 `reawebapi-webview-<arch>` 辅助程序放在 `.so` 旁边。

每个原生文件均可单独下载。各平台 ZIP 包含 Demo 和 SDK。`ReaWebAPI-ReaPack-v0.1.2.zip` 只包含 `extension/` 内的 7 个原生文件和 `ReaWebAPI.ext`，可复制到 ReaScripts 仓库。`.ext` 也提供独立下载。

运行 Demo 时，将平台 ZIP 合并到 REAPER 资源目录，在 Action List 加载 `Scripts/ReaWebAPI/Example/Example.lua`。Demo 会跟随轨道选择自动更新。声像滑块演示连续写入合并，**Center pan (Undo)** 演示 Undo 批处理，**Runtime diagnostics** 显示宿主状态。**Dock / Undock** 可切换停靠和浮动。

## API

```lua
local file = debug.getinfo(1, "S").source:sub(2)
local directory = file:match("^(.*[/\\])")
local id = reaper.ReaWebOpen(directory .. "index.html")
if id == 0 then reaper.ShowConsoleMsg(reaper.ReaWeb_GetLastError() .. "\n") end
```

Lua 提供 `ReaWeb_Close`、`ReaWeb_IsOpen`、`ReaWeb_IsReady`、`ReaWeb_Focus`、`ReaWeb_DevTools`、`ReaWeb_SetDocked`、`ReaWeb_IsDocked` 和 `ReaWeb_GetDiagnostics`，均接收窗口 ID。`SetDocked` 另接收布尔值，返回实际停靠状态。`GetDiagnostics` 返回 JSON。相对路径从 `<资源目录>/Scripts/` 解析，异步加载错误写入 REAPER 控制台。

```javascript
await reaper.ready;
const track = await reaper.GetSelectedTrack(0, 0);
if (track) console.log(await reaper.GetTrackName(track));
const unsubscribe = await reaper.ReaWeb_On('selectionchange', state => console.log(state.count));
```

页面无需导入桥接脚本。当前支持轨道数量、选择、名称，以及 `D_VOL`、`D_PAN`、`B_MUTE`、`I_SOLO` 的读写，另有窗口控制和 `GetAppVersion()`。完整列表见 [TypeScript 声明](runtime/reaper.d.ts)，也可调用 `ReaWeb_GetCapabilities()` 查询。JavaScript 窗口控制方法作用于当前页面，无需传 ID。`ReaWebOpen(path)` 从当前 HTML 所在目录解析新页面的相对路径。

工程参数目前仅支持当前工程（`0` 或 `null`）。轨道句柄只在所属窗口和工程内有效，删除轨道或切换工程后应重新获取。调用失败时 Promise 抛出带有 `code`、`message` 和可选 `details` 的错误。页面重载会丢弃旧页面队列，切换或重新载入工程后旧请求会被拒绝。请求超过 25 秒尚未开始执行时会过期，已开始的调用无法取消。超时后应核对状态，写操作不要自动重试。

通用宿主接口：

| 能力 | JavaScript |
| --- | --- |
| 就绪、诊断 | `ready`、`ReaWeb_GetDiagnostics()` |
| 窗口控制 | `ReaWeb_Focus()`、`ReaWeb_SetTitle(title)`、`ReaWeb_GetWindowState()` |
| 键盘策略 | `ReaWeb_SetKeyboardCapture(boolean)`，默认捕获，关闭后遵循 REAPER 的快捷键规则 |
| 事件 | `ReaWeb_On(name, callback)`，返回可重复调用的异步取消订阅函数 |
| 批处理、Undo | `ReaWeb_Batch(calls, { undoLabel })`，每组最多 32 个调用 |
| 连续参数 | `ReaWeb_SetTrackValueLatest(track, key, value)`，被合并的等待值返回 `superseded: true` |

事件包含 `projectchange`、`selectionchange`、`windowstatechange`，提供初始状态并合并后续变化。工程和选择事件约每 100 ms 检查一次，适合刷新界面，不用于逐条记录编辑历史。批处理会预先校验参数，并同步结束 Undo 和刷新保护。中途失败返回 `BATCH_FAILED` 及已完成的结果，不回滚已经发生的修改。连续参数合并需要显式使用对应接口，不改变普通 API 的逐次调用行为，也不自动创建拖动手势的 Undo 分组。

## 运行环境

Windows 使用 WebView2，macOS 使用 WKWebView，Linux 通过独立共享进程运行 WebKitGTK。所有窗口共用浏览器 profile。Windows/Linux 数据位于 `<资源目录>/ReaWebAPI/WebViewData/`。macOS 在该目录保存 profile ID，实际存储位置由 WebKit 管理。

浮动窗口归属 REAPER，切换停靠会保留页面和 JavaScript 状态。窗口位置、尺寸、最大化和停靠状态保存于 `<资源目录>/ReaWebAPI/WindowState/`，按页面路径和同时打开的实例序号区分。恢复时会校正超出屏幕的位置。Windows/Linux 可用 Demo 的 **Developer Tools** 按钮调试。macOS 需启用 Safari 开发者功能，再从 **Develop** 菜单检查 REAPER 页面。macOS 构建使用 ad-hoc 签名，未做公证。

REAPER API 始终在主线程执行，窗口轮流处理请求，桥接调度采用每轮 2 ms 的软预算。单个原生调用和停靠操作无法抢占。桥接 JSON 解析、序列化及状态文件写入交给工作线程，队列和消息大小均有限制。

仅加载可信的本地页面。当前桥接提供明确实现的一组 API，尚未覆盖整个 REAPER API。浏览器 profile 共用，存储 key 建议加上工具名称前缀。

## 构建与发布

需要 CMake 3.24+、C++17、Python 3 和对应平台开发工具。Linux 还需 `pkg-config`、`libwebkit2gtk-4.1-dev`、`libx11-dev`。依赖版本固定在 `CMakeLists.txt`。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix dist/install
```

Windows 使用 MSVC x64，配置时加 `-A x64`。macOS 加 `-DCMAKE_OSX_ARCHITECTURES=arm64` 或 `x86_64`。

推送到默认分支后，Actions 会构建五个平台目标，并按 `CMakeLists.txt` 中的版本发布到 **Releases**。匹配的 `v*` 标签和默认分支上的手动运行也可发布。PR 只构建。已有正式版本不会被覆盖，发布新版时递增版本号。

提交白名单会排除本地测试、依赖缓存和编译产物，CI 仅依赖已提交的源码。第三方说明见 [THIRD_PARTY.md](THIRD_PARTY.md)。
