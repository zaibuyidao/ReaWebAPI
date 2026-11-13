# ReaWebAPI

- Add opt-in App cross-origin isolation through `webRuntime.crossOriginIsolation`, retaining Windows origins and using persistent authenticated loopback origins on macOS/Linux.
- Add a private Linux WebKitGTK runtime build with isolated-process SharedArrayBuffer support and relocatable subprocesses.
- Report actual document, Worker, shared-memory and WebGL2 capabilities with cached probes and native initialization-failure diagnostics.
- Add URL/Blob Worker and atomic WebAssembly examples and regression tests.
