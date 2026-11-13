# WebAssembly 线程

在 REAPER 内运行 `reawebapi-isolation.lua`。Manifest 要求建立跨源隔离，修改策略后须关闭该 App 的全部窗口再打开。

页面报告浏览器实际能力，先后运行普通 Worker 和 Blob Worker，每组四个线程。每组共享 WASM 内存和 SAB，通过 Atomics 等待/唤醒，并执行 `atomic.wasm` 得到确定性结果 100000。源码为 `atomic.wat`。同时验证 Worker 依赖、异常、自主退出及 localStorage/IndexedDB 持久计数。结束时终止 Worker 并释放 Blob URL。

`webgl2` 独立报告，不是该计算的必要条件。此示例不代表 Emscripten 核心或音视频功能已通过验收。详见 [Web Runtime 契约](../../../docs/frontend.zh-CN.md#app-级跨源隔离)。
