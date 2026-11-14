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
