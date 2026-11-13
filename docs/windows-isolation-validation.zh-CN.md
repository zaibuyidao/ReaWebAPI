# Windows 隔离验收 — v0.3.9.43

环境为 Windows、REAPER 7.85/x64、WebView2 157.0.4341.46。浏览器专项测试使用生产扩展与独立测试宿主配置，完整入口另在独立 REAPER 实例中验证。

| 检查 | 结果 |
| --- | --- |
| 隔离、普通/Blob Worker、共享 WASM、Atomics | 通过 |
| required 模式在 App 执行前阻断失败、CSP、CORS/CORP | 通过 |
| App 来源与存储保持、重载、多窗口 | 通过 |
| DOSBox Pure 线程输入、暂停/恢复、读档 | Windows 键盘消息触发预期画面变化，暂停状态不变，读档恢复原颜色 |
| FCEUmm 单线程与线程构建 | 两种模式均通过相同行为检查，实际共享内存与选择一致 |
| PPSSPP 64 MiB RAM、输入、暂停/恢复、读档 | 使用下述本地核心修复后通过 |
| PSP WebGL2 与 Web Audio | 已验证实际 WebGL2 上下文和非零音频输出缓冲 |
| DOS 与修复后 PSP 关闭重开 | 分别重开 20 次通过，Worker 回到 0，PSP 测试进程无残留 |
| DOS 连续运行 30 分钟 | 通过（1800.1 秒），4 个 Worker，退出后无测试进程残留 |
| REAPER 中 ReaEmulator 完整入口 | 平台选择器至线程 NES 与修复后 PSP 均通过 |

EmulatorJS 4.2.3 原版 PPSSPP WASM 为 PSP 地址区分配 `0x0A000000` 字节。RAM 从 `0x08000000` 开始，因此只容纳 32 MiB。64 MiB 路径需要分配到 `0x0C000000`。原版 `cube.elf` 故障也在独立 Edge 中复现，堆损坏后的分配器越界位置为 WASM 偏移 `0x184a5`。

ReaEmulator 修复将分配常量在 `0xa19564` 处的一个字节由 `d0` 改为 `e0`，其余 WASM 字节、元数据和许可证成员不变。JavaScript 加载器另有两处修复，支持用户输入时恢复 interrupted Web Audio，并处理可选屏幕唤醒请求的拒绝。适配包保留内存修复对应源码差异、精确 JavaScript 替换、带完整校验的补丁脚本及修复前后哈希。这些是本地核心修复。ReaWebAPI 内没有 PSP 专用处理，DOSBox Pure 与 FCEUmm 仍使用原版。

PSP 使用原核心的软件渲染回退并通过 WebGL2 显示，不代表已验证 PSP 硬件加速渲染。音频验证覆盖运行中的 Web Audio 上下文及非零提交采样，不包含听测。商业游戏兼容性、实体手柄与大文件导出不计入本项验收。macOS/Linux 结果见独立的 [WebKit 验收记录](webkit-isolation-validation.md)。

复现命令见[英文记录](windows-isolation-validation.md#reproduction)。每次运行使用新的输出目录，并提供 ReaEmulator 目录与合法 PSP 测试程序。监测器记录 Worker、Windows 进程内存和退出清理。重开时浏览器进程更换造成的调试连接间隙单独标记，不把间隙当成 Worker 已清零。

稳定后进程私有内存采样均值从 492.6 MiB 到 489.6 MiB。DOS/PSP 原生关闭调用均低于本次计时的 15.625 ms 分辨率，异步进程退出另行检查。
