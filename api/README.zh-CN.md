# API Mirror 维护

[English](README.md) | **简体中文**

范围是 REAPER **7.80 的 730 个标准 C/Lua API**。这 730 项均生成真实的 C++ 调用入口，缺失绑定为 0。Lua/EEL/Python 内建函数、`gfx` 和第三方扩展不在本模块范围内，195 个相关文档锚点另存于 `excludedBuiltins`。

## 职责

| 模块 | 职责 |
| --- | --- |
| `tools/api_sync/` | 解析、规范化、比较官方定义，生成定义文件和 SDK，不修改原生实现、审阅清单或 Git |
| `tools/native_bindings.py` | 校验 Lua/C 参数对应关系，生成强类型 C++ 调用入口，显式接受已审阅的绑定契约 |
| `src/core/native.cpp`、`src/core/native_call.hpp` | 参数转换、输出缓冲区、对象句柄和资源生命周期 |
| `src/core/core.cpp`、`src/runtime/runtime.cpp` | 主线程调度、页面隔离、错误和宿主能力 |

`api/reaper_api.json` 保留原始签名、规范化参数、返回值、分类和来源 SHA-256。每项 `signatureHash` 包含 C 与 Lua 契约，`catalogueHash` 覆盖全表。文档只存链接和哈希，不整篇复制。`schemaVersion` 管理数据结构版本，`generatorVersion` 管理规范化算法版本。

`api/bindings.json` 是提交的审阅结果。`runtime/reaper-api.generated.js`、`.d.ts` 和 `docs/api-reference.md` 从定义及该清单生成，逐项参考包含签名、参数类型、返回名称及官方链接。验证会拒绝过期声明或参考文档。C++ 实现仅生成到构建目录，不重复提交生成源码。

工具作者应从 [开发文档](../docs/README.zh-CN.md) 开始。`tools/package_sdk.py` 生成独立 SDK，并将同一份 SDK 和文档加入平台包。

## 日常命令

需要 Python 3.10+，不依赖第三方 Python 包，在仓库根目录运行：

```sh
python -m tools.api_sync check --require-complete
python -m tools.api_sync report
python -m tools.api_sync verify
```

`check` 下载官方帮助并比较，不写文件。返回 0 表示一致，1 表示差异或待复核，2 表示输入错误。`--require-complete` 还要求所有绑定完成。`report` 默认离线，`--json` 输出结构化结果，`--full` 列出完整差异。`verify` 是 CMake 和 CI 的离线门禁。

`--offline` 使用已提交定义。`--source FILE_OR_HTTPS_URL` 支持官方 HTML、JSON 或 Markdown 交换格式，`--sha256 HEX` 固定输入内容。`--root PATH` 放在子命令前，可维护另一份仓库。删除 API 需要显式 `--allow-removals`，降级还需要 `--allow-downgrade`。

`update --offline` 只写入需要更新的生成文件。比较时将 Git 检出的 LF、CRLF 换行视为等价：内容未变的文件保留原始字节和修改时间，JSON 报告的 `written` 列表为空。实际内容差异仍会修复，新生成的内容采用 UTF-8 编码和 LF 换行。

## 升级官方 API

1. 执行 `check --full`，审阅新增、移除和签名变化。
2. 执行 `update`，直接更新定义，追加 `api/changelog/<版本>.md`，不会修改已审阅绑定。
3. 审阅 `tools/native_bindings.py` 的转换规则，补齐新类型及特殊约定。更新 CMake 中固定的官方 SDK 版本，保证 SDK 与定义一致。
4. 在完成审阅后，显式生成绑定清单和 SDK：

```sh
python tools/native_bindings.py --manifest
python -m tools.api_sync update --offline
python -m tools.api_sync verify
```

5. 编译各平台并验证新的参数、输出与生命周期行为，审阅 diff 后提交。

`--manifest` 会接受当前签名，因此只能在审阅后执行。普通构建不会执行它。定义更新与原生绑定审阅是两个独立步骤。

生成器要求每个 C 参数、Lua 输入和输出均有唯一映射，检查类型、长度参数、可选项和返回顺序。未识别的类型或不完整映射会使构建失败。每个生成函数还通过 `static_assert` 与固定版本的官方 `reaper_plugin_functions.h` 比较函数指针类型。签名指纹不符、SDK 不符、遗漏方法或 SDK 文件过期均不能通过构建。纯文档变化不会使签名失效。

## JavaScript 契约

- 参数及结果遵循官方 Lua 顺序。单值返回标量，多值返回数组，void 返回 `undefined`。可选参数可省略或传 `null`，中间留空不会改变后续参数位置。
- `GetTrackName` 返回 `[ok, name]`。这替代早期的单字符串接口，工程与索引参数也不再自动补默认值。
- 原生对象使用带类型的 opaque handle。`ReaProject` 接受 `0`、`null` 或 `EnumProjects` 返回的句柄，支持多个已打开工程。句柄不跨页面共享，不可由地址伪造。
- 项目对象通过 `ValidatePtr2` 和适用的 GUID 检查有效性。标记通过 GUID 重新查找，窗口使用平台窗口校验。资源创建与销毁会更新句柄注册，页面关闭或重载清理本页创建的未转交资源。
- `PCM_source` 转交 Take 后由工程管理。替换下来的源可显式销毁，仍被工程引用的源拒绝直接销毁。音频 accessor 和 joystick 可显式释放。
- 二进制参数接受 `Uint8Array`，文本也可传 UTF-8 字符串。二进制结果返回 `Uint8Array`，保留 NUL 和高位字节。适用于 MIDI、PCM sink 配置及硬件 MIDI 消息。
- 三个 `reaper.array` 接口接受 `Float64Array` 或 `number[]`，校验缓冲区容量并在 Promise 完成前回写。`Float64Array` 按小端 IEEE 754 二进制传输，保留特殊浮点值。不要在调用未完成时改变数组长度。
- GUID 使用 `{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}` 字符串，RECT 按 Lua 的四个坐标展开。可空原生字符串或 GUID 返回 `null`。
- 整数按对应 C ABI 范围校验，`size_t` 还受 JS 安全整数范围约束。普通浮点参数要求有限值。

`NeedBig` 通过 REAPER 的 `realloc_cmd_register_buf` 动态扩容并在复制结果后释放。普通固定缓冲区默认 64 KiB，可用 `reaper.debug.setBufferSize(bytes)` 调整，范围 4 KiB 到 16 MiB。MIDI 事件读取可按宿主报告的长度扩容重读。单个字符串/二进制结果上限 16 MiB，音频数组上限 1,048,576 个 double，JSON 消息上限 64 MiB。超限明确报错，不静默截断。

`reaper.transaction.batch` 接受调用数组或同步 Mirror Builder 回调，支持 `ReaWebBatchMethod` 列出的 173 个已审核接口。Builder 签名从 Mirror 声明推导，生成的多返回值数量用于解构。其他 API 使用独立异步调用。排队超时只约束尚未开始的请求，不会打断等待用户输入的对话框或长时间渲染。跨多个异步调用自行打开 Undo 或 UI refresh 保护时，应使用 `try/finally` 配对关闭。

## 运行时可用性与验证

`(await reaper.system.getCapabilities()).api` 区分两个数字：`implemented` 是编入扩展的绑定数，`available` 是当前 REAPER 能解析到的原生函数数。`unavailable` 列出宿主缺少的函数，调用时报 `API_UNAVAILABLE`。完整使用本目录对应的全部函数需要 REAPER 7.80 或更新版本。

编译验证确保 730 个函数的 ABI 类型和注册完整。开发验证另包含 730 个无副作用的模拟原生调用、JS 二进制/数组测试及真实 WebView 中的 Demo 测试。模拟调用不代表已在实际工程中执行过全部有副作用 API。发布仍需各平台真实 REAPER 回归。

Demo 的 **Run read-only checks** 覆盖 10 条调用路径，不修改工程，资源检查会在 `finally` 释放 accessor。空工程显示跳过项。只有用户点击颜色、声像和光标按钮才执行对应写入。

CMake 将定义摘要、调用入口和 JS 方法列表嵌入扩展。ReaPack 包包含 `ReaWebAPI.ext`、`extension/` 内 7 个原生文件和完整的 `web/` 示例文件夹。

## 其他输入

JSON 交换格式是完整快照：`reaperVersion` 加以 API 名称索引的 `functions`，每项包含 `signatures.c` 和 `signatures.lua`。Markdown 使用 `<!-- reaper-version: 7.80 -->`、`## API名称` 及对应 `c`、`lua` 代码块。无版本来源可用 `--source-version` 补充。局部快照会触发删除保护，不会被当作增量合并。
