# Third-party components

- [REAPER extension SDK](https://github.com/justinfrankel/reaper-sdk), Cockos Incorporated: zlib-style license; SDK header retains the complete notice.
- [WDL / SWELL](https://github.com/justinfrankel/WDL), Cockos Incorporated: zlib-style license. Used for native dockable containers on macOS and Linux.
- [nlohmann/json 3.11.3](https://github.com/nlohmann/json), Niels Lohmann and contributors: MIT.
- [Microsoft.Web.WebView2 1.0.2903.40](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.2903.40): Microsoft WebView2 SDK license. The Windows module links its static loader; the Evergreen browser runtime is installed separately.
- macOS links the system Cocoa/WebKit frameworks. The Linux helper dynamically links system GTK 3, WebKitGTK 4.1 and Xlib. These libraries are not bundled.

Dependency notices are included under `ReaWebAPI/licenses/` in installation bundles. Exact source revisions and the WebView2 package SHA-256 are pinned in `CMakeLists.txt`.
