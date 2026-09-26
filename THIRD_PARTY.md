# Third-party components

- [REAPER extension SDK](https://github.com/justinfrankel/reaper-sdk), Cockos Incorporated: zlib-style license; SDK header retains the complete notice.
- [WDL / SWELL](https://github.com/justinfrankel/WDL), Cockos Incorporated: zlib-style license. Used for native dockable containers on macOS and Linux.
- [nlohmann/json 3.11.3](https://github.com/nlohmann/json), Niels Lohmann and contributors: MIT.
- [libebur128 1.2.6](https://github.com/jiixyj/libebur128/tree/v1.2.6), Jan Kokemüller and contributors: MIT. Used for worker-thread EBU R128 loudness analysis. Its bundled queue header retains the BSD notice.
- [Microsoft.Web.WebView2 1.0.2903.40](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.2903.40): Microsoft WebView2 SDK license. The Windows module links its static loader; the Evergreen browser runtime is installed separately.
- [cpp-httplib 0.56.0](https://github.com/yhirose/cpp-httplib/tree/v0.56.0), Yuji Hirose and contributors: MIT. Used only for read-only loopback App resources; no HTTP bridge or replacement browser fetch implementation.
- macOS links the system Cocoa/WebKit frameworks. The Linux helper dynamically links system GTK 3, WebKitGTK 4.1 and Xlib. These libraries are not bundled.
- [LunaSVG 3.5.0](https://github.com/sammycage/lunasvg/tree/v3.5.0) and its pinned PlutoVG submodule, Samuel Habteselassie: MIT. Used for in-memory SVG window icons. PlutoVG includes Sean Barrett’s stb image/font code under MIT and rasterizer/stroker code under the FreeType Project License. This software is based in part on the work of the [FreeType Team](https://freetype.org/).

Dependency notices are included under `ReaWebAPI/licenses/` in platform installation bundles. Exact source revisions and the WebView2 package SHA-256 are pinned in `CMakeLists.txt`.

The API catalogue is derived from the [official REAPER ReaScript documentation](https://www.reaper.fm/sdk/reascript/reascripthelp.html), Cockos Incorporated. It stores factual API signatures and links, with source version and SHA-256 recorded in `api/reaper_api.json`. Full documentation prose is not bundled.
