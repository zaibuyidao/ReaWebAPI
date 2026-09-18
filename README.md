# ReaWebAPI

REAPER 原生扩展：在独立 WebView 窗口中运行 HTML/CSS/JavaScript，通过异步 `reaper` 对象调用 REAPER API。Lua 仅负责 Action List 入口；扩展管理 WebView、共享浏览器环境、消息桥和窗口生命周期。

当前版本 **0.1.0 / Phase 1**。实现运行时及首批 Track API，后续逐步扩展官方 API 映射。没有应用扫描、注册表或中央 App Loader。

## 获取扩展

把本仓库提交并 push 到 GitHub 后，`Build ReaWebAPI` workflow 自动编译、校验安装包内容并上传产物。打开仓库 **Actions → 对应运行 → Artifacts**，下载对应架构的包。PR 和手动 `workflow_dispatch` 同样触发；本地 commit 本身不会触发 GitHub Actions。构建失败的目标不会上传安装包。

| Artifact | 扩展文件 | 运行依赖 |
| --- | --- | --- |
| ReaWebAPI-windows-x64 | `reaper_reawebapi-x64.dll` | Windows 10/11 x64；WebView2 Evergreen Runtime；VC++ 2015–2022 x64 Runtime |
| ReaWebAPI-macos-arm64 | `reaper_reawebapi-arm64.dylib` | macOS 14+；原生 ARM64 REAPER |
| ReaWebAPI-macos-x86_64 | `reaper_reawebapi-x86_64.dylib` | macOS 14+；Intel/Rosetta x86_64 REAPER |
| ReaWebAPI-linux-x86_64 | `reaper_reawebapi-x86_64.so` | Ubuntu 24.04 或兼容系统；WebKitGTK 4.1 |
| ReaWebAPI-linux-aarch64 | `reaper_reawebapi-aarch64.so` | Ubuntu 24.04 ARM64 或兼容系统；WebKitGTK 4.1 |

Linux CI 产物以 Ubuntu 24.04 为构建基线；旧版 glibc 系统需自行编译。选择与 **REAPER 进程架构**相同的二进制。macOS 产物仅做 ad-hoc 签名，不含 Apple notarization。Actions artifact 保存 30 天，可重新运行构建。

## 安装与 Demo

1. 在 REAPER 中执行 **Options → Show REAPER resource path in explorer/finder**。
2. 退出 REAPER，将安装包内 `UserPlugins/`、`Scripts/`、`ReaWebAPI/` 合并到该资源目录；每次只安装一个匹配架构的扩展。
3. 重启 REAPER，在 Action List 中通过 **New action → Load ReaScript** 加载 `Scripts/ReaWebAPI/Example/Example.lua`。
4. 选择一条轨道，运行该 Action，点击 **Get selected track name**。

```text
REAPER resource directory/
├── UserPlugins/reaper_reawebapi-<arch>.<dll|dylib|so>
├── Scripts/ReaWebAPI/Example/
│   ├── Example.lua
│   ├── index.html
│   ├── app.js
│   └── style.css
└── ReaWebAPI/
    ├── SDK/reaper.d.ts
    └── WebViewData/          # 第一次打开窗口时创建
```

Windows 若缺少浏览器内核，安装 [Microsoft WebView2 Evergreen Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)。Linux 运行依赖：`sudo apt install libwebkit2gtk-4.1-0`。安装包不捆绑浏览器内核。

Demo 显示真实 REAPER 轨道名称、轨道数量及 API 活动；未选择轨道时显示空状态。直接在普通浏览器中打开 HTML 会提示从 REAPER 启动，不提供模拟数据。

## Lua 入口

```lua
local script = debug.getinfo(1, "S").source:sub(2)
local directory = script:match("^(.*[/\\])")
local window = reaper.ReaWebOpen(directory .. "index.html")
if window == 0 then
  reaper.ShowConsoleMsg(reaper.ReaWeb_GetLastError() .. "\n")
end
```

`ReaWebOpen(path)` 接收 UTF-8 本地 `.html` / `.htm` 文件路径，返回正整数窗口 ID，失败返回 `0`。Lua 相对路径从 **REAPER 资源目录的 `Scripts/`** 解析；不依赖进程工作目录，也不推断当前 Lua 文件位置。入口脚本应使用上面的绝对路径写法。

```lua
reaper.ReaWebOpen("SendFlow/index.html") -- <resource>/Scripts/SendFlow/index.html
reaper.ReaWeb_IsOpen(window)            -- boolean，包含异步初始化阶段
reaper.ReaWeb_DevTools(window)          -- Windows/Linux 打开检查器
reaper.ReaWeb_Close(window)             -- 请求关闭，下一次主线程 timer 清理
reaper.ReaWeb_GetLastError()            -- 最近一次运行时错误
```

WebView2 初始化异步完成。返回 ID 表示窗口请求已接受；后续初始化错误写入 REAPER console，并使窗口关闭。无需 Lua `defer()` 维持窗口。

## JavaScript API

扩展在页面脚本执行前注入 `window.reaper`；无需导入 `reaper.js`。所有方法返回 Promise。

```javascript
const track = await reaper.GetSelectedTrack(0, 0);
if (track) {
  const name = await reaper.GetTrackName(track);
  console.log('Selected Track:', name);
}
```

| 方法 | 返回值 / 范围 |
| --- | --- |
| `CountTracks(project = 0)` | 轨道数量，不含 master |
| `CountSelectedTracks(project = 0)` | 选中轨道数量 |
| `GetTrack(project, index)` | 轨道句柄或 `null`；零基索引 |
| `GetSelectedTrack(project = 0, index = 0)` | 轨道句柄或 `null` |
| `GetTrackName(track)` | 名称字符串；按需求示例折叠原生 bool + 输出缓冲区 |
| `GetMediaTrackInfo_Value(track, key)` | 数值 |
| `SetMediaTrackInfo_Value(track, key, value)` | boolean；不自动包装 undo block |
| `GetAppVersion()` | REAPER 版本字符串 |
| `ReaWebOpen(path)` | 新窗口 ID；相对当前 HTML 所在目录解析 |
| `ReaWeb_Close()` | 关闭当前窗口；页面销毁后不保证 Promise 可继续执行 |
| `ReaWeb_DevTools()` | 打开当前窗口的开发者工具，见 macOS 说明 |
| `ReaWeb_GetCapabilities()` | `{ version, methods, projectScope }` |

`project` 当前仅接受 `0` / `null`，表示当前工程。参数默认值仅用于上表标明的方法。轨道参数支持 `D_VOL`（线性增益 ≥ 0）、`D_PAN`（-1…1）、`B_MUTE`（0/1）、`I_SOLO`（0/1/2）。未实现的官方 API 不会被自动透传。声明见 [runtime/reaper.d.ts](runtime/reaper.d.ts)。

句柄只在所属窗口和当前工程中有效，不包含原生地址。每次使用前验证类型、工程、`ValidatePtr2` 和 Track GUID，拒绝失效对象；不要持久化句柄，工程切换/删除轨道后重新获取。Bridge 失败时 Promise 拒绝，错误带有 `code` 和 `message`。

## WebView 环境与调试

- **Windows**：扩展只创建一个 `ICoreWebView2Environment`，所有 controller 复用它；UDF 固定为 `<resource>/ReaWebAPI/WebViewData/`。窗口关闭时不退出 REAPER 的消息循环。Demo 的 Developer Tools 按钮打开 Console/Elements。
- **Linux**：所有窗口共用一个 `WebKitWebContext` 和 `WebKitWebsiteDataManager`；浏览器数据和 cache 放在 `WebViewData/`。GTK 非阻塞事件处理接入 REAPER timer，不启动第二个主循环。支持 WebKit Inspector。
- **macOS**：所有窗口共用同一个持久化 `WKWebsiteDataStore` profile 及 `WKProcessPool`；profile UUID 保存在 `WebViewData/wk-profile-id`。**WKWebView 公共 API 不支持把完整浏览器存储重定向到任意 UDF 路径，实际缓存由 WebKit 管理**。这是 v3 指定目录要求的平台差异。

macOS 使用公开的 `inspectable` API。开启 Safari **Settings → Advanced → Show features for web developers**，再从 **Develop → 本机 → REAPER 页面**进入 Web Inspector。`ReaWeb_DevTools()` 会返回包含操作路径的 `INSPECTOR_MENU` 错误；没有使用私有 WebKit API 强行弹出 Inspector。[Apple inspection 文档](https://developer.apple.com/documentation/safari-developer-tools/enabling-inspecting-content-in-your-apps) · [WebKit profile API](https://webkit.org/blog/14423/building-profiles-with-new-webkit-api/)

所有应用共用浏览器 profile，不承诺应用间存储隔离；存储 key 应使用工具名前缀。工具不得自行创建 WebView2 environment。

## 构建

依赖版本在 `CMakeLists.txt` 固定。首次 configure 需要访问 GitHub 和 Windows 下的 NuGet。

```powershell
# Windows，Visual Studio 2022 C++ toolchain + Windows SDK
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix dist/install
```

```sh
# macOS；Intel 版本改为 x86_64
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0

# Linux
sudo apt install build-essential cmake pkg-config libwebkit2gtk-4.1-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 共用步骤
cmake --build build --parallel
cmake --install build --prefix dist/install
```

默认只构建扩展，不需要 Node.js。安装包校验脚本 `tools/package.py` 使用 Python 3。WebView2 loader 静态链接，无需额外分发 `WebView2Loader.dll`。

`.gitignore` 按发布内容白名单管理：只提交源码、JavaScript SDK、Demo、构建工作流、打包脚本和必要说明。本地测试、验收记录、下载依赖、编译产物及安装 ZIP 不进入仓库；GitHub Actions 不依赖这些本地文件。

## 实现与边界

`src/core.cpp` 是显式 API 注册与 JSON 参数转换层；`src/plugin.cpp` 加载 REAPER 函数并注册 Lua API；`src/runtime.cpp` 负责窗口会话和主线程消息队列；`src/platform_*` 实现三种浏览器后端。

扩展 API 时，在 Host 添加类型明确的函数、在 Bridge 注册方法和参数校验，再补充 `runtime/reaper.js`、`.d.ts` 和测试。仅 Track 句柄已实现；Project/Item/FX/Send/Envelope/Marker/Media 的完整镜像属于后续阶段。

只加载可信本地工具。桥接没有任意原生调用、shell 或通用函数地址入口；顶层导航限制为入口页面，弹窗被阻止。它是本地脚本运行环境，**不是不可信网页的安全沙箱**。32 窗口、每窗口 256 条待处理消息、64 KiB 消息、64 层 JSON、每次 timer 每窗口最多 32 个 API 调用；Promise 30 秒超时。超时不会撤销已经排队的原生调用，写操作不应自动重试。

Windows/Linux x64 已完成本地编译及模拟宿主验证；真实 REAPER 中的焦点、输入法、播放并行、多窗口和 Inspector 仍需实机验收。macOS 和 Linux ARM 构建由对应 GitHub Actions 任务执行。
