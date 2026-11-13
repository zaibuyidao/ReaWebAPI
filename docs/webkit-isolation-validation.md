# WebKit isolation acceptance — v0.3.9.43

| Environment | Engine | Result |
| --- | --- | --- |
| macOS 26.0.1, ARM64, REAPER 7.85 | System WKWebView 21622.1.22.11.15 | Functional matrix and 30-minute DOS run passed |
| Ubuntu 24.04.1, x86_64, WSL | Private WebKitGTK 2.54.2 | Functional matrix and 30-minute DOS run passed |

Both platforms use the production extension in disposable REAPER profiles. Required and off modes each passed 20 reopens. Checks cover authenticated stable loopback origins, simultaneous windows, localStorage/IndexedDB, reload, four URL and four Blob Workers, shared WASM atomics, wait/notify, CSP Worker restrictions, CORS/CORP and COEP. Native resource tests cover authorization, Host/Origin checks, persistent port conflicts, MIME types, ranges and per-window media lookup. The Linux engine retains the sandbox and enables shared memory only in isolated processes.

Both platforms passed DOSBox Pure input, animation, pause/resume, save/load and nonzero audio buffers, FCEUmm single/threaded behavior, and corrected PPSSPP 64 MiB, controller, state restoration, WebGL2 and audio fixtures. DOS ran for 1800.1 seconds on macOS and 1800.1 seconds on Linux, each followed by 20 reopens. PSP passed 20 reopens. The complete ReaEmulator entry passed threaded NES and PSP selection and rendering on both platforms. The final PSP loader passed another 20 reopens on each platform. Linux also passed the isolation matrix using the relocated runtime from the review package. Native keyboard events were trusted browser events. Core screenshot fixtures preserve the drawing buffer for deterministic asynchronous readback.

During the macOS DOS run, WASM memory remained 128 MiB. Web-process RSS increased from 538.8 MiB after approximately two minutes to 566.8 MiB at the end. The run completed without a stall or crash, but this interval does not establish a long-term memory plateau. Sampled Web processes were absent after shutdown. Worker instrumentation counts constructors minus explicit terminations, including self-closed Workers, and is not a live engine-thread census.

Linux DOS also retained 128 MiB WASM memory. Mean Web-process RSS after warmup changed from 636.5 to 650.9 MiB at the end. No sampled test children remained after shutdown. These RSS measurements are not directly comparable with Windows private-memory measurements. Linux used WSL, Xvfb and native XTest input, not a physical desktop.

The tested macOS engine rejects WASM instantiation under restrictive CSP but permits byte compilation. This matches the [upstream WebKit issue and fix](https://github.com/WebKit/WebKit/commit/4003087f20b3fb38523cd7c92c804ba44d9405fc). Both results are retained in the policy report. ReaWebAPI does not override CSP or replace system WKWebView. Linux's tested engine rejects compilation too.

An independent system WKWebView control reproduces the defect without loading ReaWebAPI. Under a restrictive HTTP CSP, `compile`, `Module` construction and `compileStreaming` succeed in the document, URL Worker and Blob Worker. Instantiation rejects with `CompileError`. With `wasm-unsafe-eval`, all tested entry points succeed. Emulator pages permit WASM, so this defect does not invalidate their functional results on 26.0.1. Correct enforcement requires an engine correction, not a response-header change. The control reports are retained with the acceptance evidence.

The local PSP loader handles both `suspended` and `interrupted` Web Audio states on user input, and handles rejection of optional screen wake-lock requests. Browser permissions remain enforced. Linux tests use native button/key events to start and resume audio. No threading or browser API is replaced.

## Follow-up memory sampling

The same DOS fixture ran for a further 2198.7 seconds on macOS and 2081.1 seconds on Linux. Sampling ended through normal REAPER closure after the requested 30-minute threshold. Input/state checks passed before sampling, frames and audio advanced throughout, WASM remained 128 MiB, and sampled processes exited. This run did not complete the originally configured 60-minute interval or repeat the earlier reopen matrix.

On macOS, physical-footprint checkpoints were 255.9, 243.7, 245.3 and 245.1 MiB, with a final reading of 301.7 MiB. `leaks` reported the same five objects totaling 1680 bytes before and after the run. An independent WKWebView process without ReaWebAPI reported the identical object types and byte count. This identifies a small engine-side baseline, rather than growth attributable to ReaWebAPI in this comparison. Native leak scans do not establish the absence of retained JavaScript objects.

On Linux, mean Web-process private memory was 533.1 MiB during minutes 10–20, 536.9 MiB during minutes 20–30 and 542.8 MiB during the remaining interval. Values fluctuated and fell between peaks, but the increasing interval means do not establish a plateau or exclude a slow leak. REAPER RSS remained approximately 176.4 MiB. No application-specific allocation leak was isolated, so no speculative core or memory-management change was applied.

The follow-up adds optional `--memory-diagnostics` sampling to the core test driver. It records Linux private/PSS memory and macOS `vmmap` checkpoints. macOS remains on 26.0.1. No OS-version gate or WASM-deny policy was added to the runtime or ReaEmulator.

These results do not cover macOS Intel, Linux ARM64, other Linux distributions, physical controllers, listening tests or PSP hardware rendering. PSP uses the core's software fallback with WebGL2 presentation. Large exports remain limited to 16 MiB per bridge call. Required mode on macOS/Linux uses separate browser storage, retaining old storage without automatic migration.

## Reproduction

Use a new output directory and legally obtained PSP fixtures. Linux requires the [private runtime](linux-webkit-runtime.md) beside the helper and an active X11/XWayland session. macOS requires an unlocked desktop and the native keyboard test plugin for input tests.

```sh
python3 tests/webkit_isolation_reaper.py --reaper /absolute/path/to/reaper \
  --extension /absolute/path/to/extension --expect-isolated \
  --output /new/isolation-acceptance-profile
python3 tests/isolation_webkit_cores.py --reaper /absolute/path/to/reaper \
  --extension /absolute/path/to/extension --emulator /path/to/ReaEmulator \
  --core dosbox_pure --soak-seconds 1800 --reopen 20 --output /new/core-profile
```

On macOS add `--keyboard-probe /path/to/reaper_keyboard_test.dylib`. Retain `report.json`, `resources.json`, policy reports and process logs with the reviewed binaries.

## 简体中文

macOS ARM64 和 Ubuntu x64（WSL/Xvfb）已通过隔离、存储、安全策略、原子运算及模拟器功能矩阵。DOS 连续运行 30 分钟，随后重开 20 次，PSP 另重开 20 次。完整 ReaEmulator 入口的线程 NES 和 PSP 均通过。DOS 共享内存保持 128 MiB，Web 进程驻留内存约从 539 MiB 增至 567 MiB，不能据此宣称更长运行周期内内存不增长。退出后采样到的 Web 进程无残留。

当前 Mac 系统 WebKit 的 CSP 在 WASM 实例化阶段阻断，字节编译阶段存在上述上游已知缺口。Linux DOS 连续运行 1800.1 秒，随后重开 20 次，PSP 另重开 20 次，全部通过。Linux Web 进程 RSS 预热后均值为 636.5→650.9 MiB。两平台均通过正式 PSP 加载器及完整入口复验，退出无采样测试进程残留。不把 API 存在、响应头设置或构建成功等同于验收通过。

补充内存复验实际运行约 36.6 分钟和 34.7 分钟，达到要求后正常关闭，采样进程均已退出。Mac 原生工具前后均报告 1680 字节，独立系统 WKWebView 对照也有完全相同的结果。Linux 后段私有内存均值仍小幅上升，尚不能排除慢性泄漏。没有定位到可归因于应用的分配泄漏，因此未修改模拟器核心或加入内存回收补丁。模拟器继续允许 WASM，系统版本未升级，也未添加版本锁定。
