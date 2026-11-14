# Linux 隔离 WebKit 运行库

required App 同时需要隔离的 HTTP 文档及隔离 Web 进程中的 SharedArrayBuffer 支持。独立运行库使用 WebKitGTK 2.54.2，回移[上游修复 104506cd](https://github.com/WebKit/WebKit/commit/104506cdb2c45aa547db1eb50313abc15473b815)的四行生产代码，仅对 GTK/WPE 隔离进程启用共享内存。本地路径补丁允许发布构建使用 `WEBKIT_EXEC_PATH`，无需开启开发模式。不全局设置 `JSC_useSharedArrayBuffer`，不关闭 WebKit 沙箱。

在 Linux 上使用 Clang 18、CMake、Ninja、Python 3.12+、Ruby、Bison、Flex、gperf，以及 WebKitGTK 所需的 GTK 3 / libsoup 3 开发依赖构建。构建脚本固定源码压缩包校验值及回移补丁。构建需要较多时间和内存。

```sh
python3 tools/build_webkit_runtime.py --work /path/to/webkit-build \
  --output /path/to/reawebapi-webkit-x86_64 --jobs 4
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DREAWEB_WEBKIT_RUNTIME=/path/to/reawebapi-webkit-x86_64
cmake --build build --parallel 4
cmake --install build --prefix stage
```

将 `UserPlugins/reawebapi-webkit-<arch>/` 保留在匹配的辅助程序旁。扩展仅为其辅助进程设置动态库和 WebKit 子进程路径，不修改 REAPER 或系统 WebKit。目录不存在时使用系统 WebKit，目录不完整时明确报告启动错误。运行库依赖与发行版相关，应分别验证各支持的发行版和架构。

目录包含版本及源码元数据、校验值、上游许可证和可复现构建脚本。分发时保留这些文件，并为二进制版本提供对应源码压缩包及补丁。未指定 `REAWEB_WEBKIT_RUNTIME` 的安装包仍取决于系统 WebKit 的实际能力。
