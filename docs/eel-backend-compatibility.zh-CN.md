# EEL2 后端支持

[English](eel-backend-compatibility.md) | **简体中文**

ReaWebAPI 的 16 个脚本 Host API 可由 EEL2 和 Lua 调用。v0.3.9.14 修复了 `ReaWeb_Open` 可选第四参数的绑定，Lua 使用 `false/true`，EEL2 使用 `0/1`。省略参数仍表示 `false`。原生 C/C++ 调用保持 `const bool*` 签名，省略时传入 `nullptr`。

## 调用约定

REAPER 6.24+ 支持直接调用扩展函数。旧式 `extension_api("ReaWeb_Name", ...)` 允许先用 `APIExists` 检查扩展是否存在。直接调用未注册的函数会导致编译失败。参见[官方 ReaScript 文档](https://www.reaper.fm/sdk/reascript/reascripthelp.html#extension_api)。

```eel
single = ReaWeb_Open(#path, #key, "", 0);
extra = ReaWeb_Open(#path, #key, "", 1);
ReaWeb_IsReady(single) ? (
  ReaWeb_Send(single, "hello");
  ReaWeb_Receive(#message, single);
);
ReaWeb_GetLastError(#error);
```

| EEL2 调用 | 约定 |
| --- | --- |
| `ReaWeb_Open(#path, optional #key, optional #id, optional multiple)` | 返回窗口 ID，失败为 `0`。`0` 复用 `(key,id)`，`1` 新建独立窗口。未提供 key 时始终新建 |
| `ReaWeb_OpenEx(#path, #json)` | 选项与 Lua 相同。JSON 布尔值必须为 `true/false`，不能用 `0/1` |
| `ReaWeb_OpenDev(#url)` | 可信本机 HTTP 开发页面 |
| `ReaWeb_Close(id)` | 关闭窗口 |
| `ReaWeb_IsOpen(id)`、`ReaWeb_IsReady(id)` | 数值布尔值 |
| `ReaWeb_Show(id)`、`ReaWeb_Focus(id)` | 显示或聚焦窗口 |
| `ReaWeb_SetDocked(id, docked)`、`ReaWeb_IsDocked(id)` | 实际停靠状态，成功取消停靠返回 `0` |
| `ReaWeb_DevTools(id)` | 请求原生开发工具，受平台可用性限制 |
| `ReaWeb_Send(id, #text)` | 就绪后将 UTF-8 文本加入队列 |
| `ReaWeb_Receive(#text, id)` | 第一个输出字符串接收下一条消息或空文本 |
| `ReaWeb_SetFrame(id, #text)` | 替换 `frame.lua` 文本，空文本清除内容 |
| `ReaWeb_GetDiagnostics(#json, id)` | 第一个输出字符串接收诊断 JSON |
| `ReaWeb_GetLastError(#error)` | 第一个输出字符串接收最近的原生错误 |

`const char*` 返回值在原生参数**之前**增加字符串输出参数。不要将函数的数值返回值赋给字符串，也不要传入缓冲区长度。EEL2 布尔值使用数值，没有 Lua `nil` 占位符。可省略尾部参数，或为可选 key/id 传入空字符串。

## 限制

- EEL2 的字符串构造限制与 Lua 不同。已测 REAPER 版本中，`str_setlen` 可构造最多 65,536 字节，而原生返回的 1 MiB 消息可完整接收并转发。这不代表消息桥存在统一的 64 KiB 上限。应用应保持消息精简，并遵守 [Host 队列限制](host-api.zh-CN.md#lua-消息桥接)。
- `ReaWeb_Receive` 用空文本表示空队列或空消息，应用协议应使用非空消息。不支持原始 NUL。
- 使用 `defer` 轮询，通过 `ReaWeb_IsOpen` 在关闭后结束，并用 `atexit` 清理。EEL2 不提供额外的工作线程或音频线程执行能力。
- EEL2 帧内容仍使用 `frame.lua` 字段。仅供原生调用的 service、stream、task C API 不会自动成为 ReaScript 函数。
- WebView、停靠和 DevTools 的平台限制与 Lua 相同。EEL2 不提供 Lua table 或内置 JSON 处理设施。

SDK 包含[最简请求响应示例](../web/eel-backend/README.zh-CN.md)和[双向轨道混音示例](../web/eel-mixer/README.zh-CN.md)，仅使用 EEL2、HTML、CSS 和 JavaScript。两个示例均使用 `ReaWeb_SetFrame` 刷新状态，音量范围为 `0..4`，推子写入具有执行确认和未确认请求限流。帧状态可以被新状态替换，执行确认仍通过消息通道传递。

## 验证

v0.3.9.14 已在 Windows x64 / REAPER 7.83、macOS ARM64 / REAPER 7.82、Linux x64 / REAPER 7.82 上验证。结果覆盖下列场景，不代表所有参数组合均已测试。

隔离宿主测试覆盖直接调用、旧式调用、Lua 对照、可选参数、窗口复用、全部 16 个脚本接口、UTF-8 消息、帧内容，以及示例 UI 和生命周期。每次使用新的输出目录：

```text
python tests/eel_api_compatibility.py --reaper <executable> --extension <native-library> --output <new-directory> --suite full --direct
python tests/eel_api_compatibility.py --reaper <executable> --extension <native-library> --output <new-directory> --suite strings
python tests/eel_backend_smoke.py --reaper <executable> --extension <native-library> --output <new-directory> --demo eel-mixer
```

省略 `--direct` 测试 `extension_api`，省略 `--demo` 测试最简示例。测试需要交互桌面。Lua 测试代码仅准备临时工程并检查结果，应用消息由交付的 EEL2 代码处理。
