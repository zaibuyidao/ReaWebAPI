# ReaWebAPI

- 新增 App 可选跨源隔离配置 `webRuntime.crossOriginIsolation`，Windows 保留原来源，macOS/Linux 使用持久绑定且需认证的回环来源。
- 新增 Linux 独立 WebKitGTK 运行库构建，支持隔离进程共享内存与可重定位的子进程。
- 新增文档、Worker、共享内存与 WebGL2 实际能力报告，提供缓存探针及原生初始化失败诊断。
- 新增普通/Blob Worker 与原子 WebAssembly 示例和回归测试。
