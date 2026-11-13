# Linux isolated WebKit runtime

Required Apps need both an isolated HTTP document and SharedArrayBuffer support in that isolated Web process. The private runtime builds WebKitGTK 2.54.2 with the four production lines from [upstream fix 104506cd](https://github.com/WebKit/WebKit/commit/104506cdb2c45aa547db1eb50313abc15473b815). It enables shared memory only for GTK/WPE isolated processes. A local relocation patch honors `WEBKIT_EXEC_PATH` in release builds without enabling developer mode. It does not set `JSC_useSharedArrayBuffer` globally or disable the WebKit sandbox.

Build on Linux with Clang 18, CMake, Ninja, Python 3.12+, Ruby, Bison, Flex, gperf and WebKitGTK's GTK 3 / libsoup 3 development dependencies. The source archive checksum and exact backport are pinned in the build script. Allow substantial build time and memory.

```sh
python3 tools/build_webkit_runtime.py --work /path/to/webkit-build \
  --output /path/to/reawebapi-webkit-x86_64 --jobs 4
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DREAWEB_WEBKIT_RUNTIME=/path/to/reawebapi-webkit-x86_64
cmake --build build --parallel 4
cmake --install build --prefix stage
```

Keep `UserPlugins/reawebapi-webkit-<arch>/` beside the matching helper. The extension sets library and WebKit subprocess paths only for its helper. REAPER and the system WebKit installation are not changed. A missing runtime directory uses system WebKit. An incomplete directory is an explicit startup error. Runtime dependencies remain distribution-specific, so validate the bundle on each supported distribution and architecture.

The directory contains version/source metadata, checksums, upstream license texts and the reproducible build script. Retain these when distributing it and provide the matching source archive and patch with binary releases. Normal packages built without `REAWEB_WEBKIT_RUNTIME` still depend on system WebKit's actual capabilities.

## 简体中文

required App 同时需要隔离的 HTTP 文档及隔离进程中的 SharedArrayBuffer 支持。独立运行库使用 WebKitGTK 2.54.2，并回移上述上游修复的四行生产代码，仅对 GTK/WPE 隔离进程启用共享内存，不全局打开共享内存或关闭安全沙箱。本地路径补丁允许发布构建使用 `WEBKIT_EXEC_PATH`，无需开启开发模式。

按上述命令构建，将 `reawebapi-webkit-<arch>/` 保留在匹配的辅助程序旁。只为辅助进程设置运行库路径，不修改 REAPER 或系统 WebKit。目录不存在时使用系统引擎，目录不完整时明确报错。未指定 `REAWEB_WEBKIT_RUNTIME` 的安装包仍取决于系统引擎的实际能力。运行库依赖与发行版相关，各发行版和架构应分别验收。分发时保留版本、校验、许可证及构建脚本，并提供对应源码和补丁。
