# ReaWebAPI

[English](README.md) | **简体中文**

REAPER 6.68+ 原生扩展，在可停靠的 WebView 中运行本地 HTML/CSS/JavaScript。Lua 负责打开页面，JavaScript 通过注入的 `reaper` 对象异步调用原生 API。

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

每个原生文件均可单独下载。各平台 ZIP 包含 Demo 和 SDK。`ReaWebAPI-ReaPack-v0.1.6.zip` 只包含 `extension/` 内的 7 个原生文件和 `ReaWebAPI.ext`，可复制到 ReaScripts 仓库。`.ext` 也提供独立下载。

将平台 ZIP 合并到 REAPER 资源目录，在 Action List 加载 `Scripts/ReaWebAPI/Example/Example.lua`。Demo 提供工程、轨道和 FX 查询，颜色及声像 Undo 操作，以及停靠切换。**Run read-only checks** 执行 10 条检查，覆盖对象句柄、多返回值、GUID、RECT、MIDI 字节和音频数组。选中带 MIDI Item 的轨道可覆盖全部路径，空工程会明确显示跳过项。

## 开发资料

[SDK 与最小模板](runtime/README.zh-CN.md) · [开发指南](docs/development.zh-CN.md) · [730 项 API 参考](docs/api-reference.md) · [宿主接口](docs/host-api.zh-CN.md)

独立下载 `ReaWebAPI-SDK-v<版本>.zip` 可获得编辑器声明、可运行模板、API 定义和文档。平台 ZIP 也包含这些内容，扩展专用 ReaPack ZIP 保持原有结构。

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
if (track) {
  const [ok, name] = await reaper.GetTrackName(track);
  if (ok) console.log(name);
}
const [total, markers, regions] = await reaper.CountProjectMarkers(0);
const [beat, bar] = await reaper.TimeMap2_timeToBeats(0, await reaper.GetCursorPosition());
```

页面无需导入桥接脚本。**REAPER 7.80 的 730 个标准 API 均已绑定原生调用**，参数和返回值按 Lua 签名排列。单值返回标量，多值返回数组，无返回值得到 `undefined`。声明见 [SDK](runtime/reaper-api.generated.d.ts)。较旧 REAPER 缺少的函数会报 `API_UNAVAILABLE`，可通过 `ReaWeb_GetCapabilities().api` 的 `available`、`unavailable` 查询。要使用全部 730 项，请使用 REAPER 7.80 或更新版本。

`0` 或 `null` 表示当前工程，`EnumProjects` 返回的工程句柄可用于其他已打开工程。轨道、Item、Take、包络及资源均使用类型化句柄，不传裸指针。删除对象或重载页面后需重新获取。MIDI 字节使用 `Uint8Array`，音频缓冲区使用 `Float64Array` 或 `number[]`，在 `await` 完成后读取回写数据。GUID 使用字符串，RECT 按官方 Lua 的四个坐标参数展开。

**升级注意：** `GetTrackName` 现在返回 `[ok, name]`，工程和索引参数应按官方签名显式传入。原来的 5 个轨道参数白名单已移除，普通调用支持 REAPER 提供的参数键。

JavaScript 窗口控制方法作用于当前页面，无需窗口 ID。`ReaWebOpen(path)` 从当前 HTML 目录解析相对路径。失败会抛出带 `code`、`message` 和可选 `details` 的错误。工程切换后旧请求会被拒绝，未开始的请求在 25 秒后过期。原生调用开始后会停止排队计时，允许对话框和渲染正常完成。已经开始的调用不能取消，超时后不要自动重试写操作。

通用宿主接口：

| 能力 | JavaScript |
| --- | --- |
| 就绪、诊断 | `ready`、`ReaWeb_GetDiagnostics()` |
| 窗口控制 | `ReaWeb_Focus()`、`ReaWeb_SetTitle(title)`、`ReaWeb_GetWindowState()` |
| 键盘策略 | `ReaWeb_SetKeyboardCapture(boolean)`，默认捕获，关闭后遵循 REAPER 的快捷键规则 |
| 事件 | `ReaWeb_On(name, callback)`，返回可重复调用的异步取消订阅函数 |
| 批处理、Undo | `ReaWeb_Batch(calls, { undoLabel })`，每组最多 128 个调用 |
| 固定输出缓冲区 | `ReaWeb_SetBufferSize(bytes)`，默认 64 KiB，最大 16 MiB |
| 连续参数 | `ReaWeb_SetTrackValueLatest(track, key, value)`，被合并的等待值返回 `superseded: true` |

事件包含轨道、Item、Take 选择，以及播放、FX、工程和窗口状态。批处理扩展至 173 个已审核标准 API，支持结果引用、当前工程校验及 Undo/刷新清理。托管 Undo 可跨 await，并由宿主在重载、关闭或超时后结束。新增统一文件、剪贴板和外链接口，以及 TypeScript/Vite loopback 开发入口。详见[宿主参考](docs/host-api.zh-CN.md)、[前端约定](docs/frontend.zh-CN.md)和 [v0.1.6 变更](docs/release-notes.md)。

## 运行环境

Windows 使用 WebView2，macOS 使用 WKWebView，Linux 为每个 App 使用独立 WebKitGTK 进程。本地 App 使用稳定的回环 HTTP 来源，直接支持 ES module 和本地 fetch。同一 App 目录的窗口共用 profile，不同目录隔离。profile 元数据位于 `<资源目录>/ReaWebAPI/Apps/<appId>/`。持久化、旧数据迁移及平台要求见 [Web Runtime v1 约定](docs/frontend.zh-CN.md)。

浮动窗口归属 REAPER，切换停靠会保留页面和 JavaScript 状态。窗口位置、尺寸、最大化和停靠状态保存于 `<资源目录>/ReaWebAPI/WindowState/`，按页面路径和同时打开的实例序号区分。恢复时会校正超出屏幕的位置。Windows/Linux 可用 Demo 的 **Developer Tools** 按钮调试。macOS 需启用 Safari 开发者功能，再从 **Develop** 菜单检查 REAPER 页面。macOS 构建使用 ad-hoc 签名，未做公证。

REAPER API 始终在主线程执行，窗口轮流处理请求，桥接调度采用每轮 2 ms 的软预算。单个原生调用和停靠操作无法抢占。桥接 JSON 解析、序列化及状态文件写入交给工作线程，队列和消息大小均有限制。

页面拥有所绑定 API 的完整能力，包括工程写入和文件操作，请仅加载可信页面。浏览器存储按 App 目录隔离，原生文件系统访问仍保留已开放权限。

## API 定义维护

[API Sync Tool](api/README.zh-CN.md) 独立维护定义与差异，`tools/native_bindings.py` 负责原生参数映射和构建期代码生成。CMake 离线检查全部定义、绑定和官方 SDK 类型，任何未映射参数或签名漂移都会阻止构建。运行时无需额外安装 JSON。

```sh
python -m tools.api_sync check --require-complete
python -m tools.api_sync report
```

API Sync 的 `update` 不自动接受绑定变更。升级官方定义的审阅流程见维护文档。

## 构建与发布

需要 CMake 3.24+、C++17、Python 3.10+ 和对应平台开发工具。Linux 还需 `pkg-config`、`libwebkit2gtk-4.1-dev`、`libx11-dev`。依赖版本固定在 `CMakeLists.txt`。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix dist/install
```

Windows 使用 MSVC x64，配置时加 `-A x64`。macOS 加 `-DCMAKE_OSX_ARCHITECTURES=arm64` 或 `x86_64`。

推送到默认分支后，Actions 会构建五个平台目标，并按 `CMakeLists.txt` 中的版本发布到 **Releases**。匹配的 `v*` 标签和默认分支上的手动运行也可发布。PR 只构建。已有正式版本不会被覆盖，发布新版时递增版本号。

回归测试已纳入仓库，各平台 CI 必须通过测试才会打包/发布。依赖缓存和构建产物仍不入库。使用 `-DBUILD_TESTING=ON` 配置后，运行 `ctest --test-dir build -C Release --output-on-failure`。第三方依赖见 [THIRD_PARTY.md](THIRD_PARTY.md)。
