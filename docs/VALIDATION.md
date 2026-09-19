# v0.1.6 validation — 2026-09-19

| Target | Result |
| --- | --- |
| Standard REAPER 7.80 mirror | 730 definitions, 730 reviewed bindings and all 730 typed native call/return mappings verified |
| Windows x64 / MSVC 19.44 | Release build; 8 CTest suites passed, including 17 JS cases, 19 Python cases and 3 actual HTTP resource contract cases |
| WebView2 153 / mock REAPER | Demo with MIDI/audio buffers, modern production template, batch/managed Undo, 100 KB binary files, docking and Vite CSS HMR passed |
| WebView2 Web Runtime v1 | All 9 required groups and 8 exercised optional groups passed; same-App reopening and two independent host processes retained localStorage/cookies/IndexedDB and the same origin; separate App directories started with independent storage |
| Linux x86_64 / Ubuntu 24.04 / GCC 13.3 | Release build; 7 native/Python/HTTP CTest suites passed; JS VM suite ran on Windows because this WSL build cache locates Windows node.exe |
| WebKitGTK 2.52.6 / WSLg | Same 17 Web Runtime check groups passed with `WEBKIT_DISABLE_DMABUF_RENDERER=1`, including nonzero viewport, animation frames, WebGL, HTTP CORS, WebSocket round-trip, both Workers, storage isolation and browser process restart |
| WebKitGTK embedding/transport | Two windows, large 100,000-byte JS request, reload, reparenting after original parent destruction and independent close passed |
| SDK | Strict TypeScript contracts, Vite build, SDK links and UTF-8 file/checksum packaging passed |
| macOS arm64/x86_64, Linux aarch64 | Sources and CI targets provided; not compiled or run locally |

The WSLg default DMA-BUF renderer stalled native animation frames; the documented WebKit environment switch fixed the graphics path. The extension does not force this switch and does not replace requestAnimationFrame. Linux viewport allocation was also corrected for a foreign X11 parent. macOS uses localhost instead of an IP-literal URL for ATS compatibility, but still requires validation against real REAPER on macOS.

HTTP tests exercise actual compiled resource servers: MIME/Unicode/query paths, HEAD/ranges, directory traversal and symlink escape rejection, Host/Origin restrictions, read-only methods, stable port restoration and port exclusivity. API tests use independent native stubs and a mock REAPER host; they do not prove every API's project side effects.

Physical OS file drops and actual REAPER drag gestures remain manual acceptance cases; automated checks dispatch standard DOM events. External TLS endpoints were not tested; network fixtures verify HTTP CORS and native WebSocket echo. Optional GPU/network capabilities remain platform dependent.

The local Windows/Linux/SDK ZIPs and SHA-256 manifests are generated under `dist/v0.1.6/` with revision `local-v0.1.6`. No project license, custom Lua services or third-party REAPER API registration was added. Third-party C++ dependency notices are preserved. No GitHub push, tag or publication was performed.

Run [SMOKE_TEST.md](SMOKE_TEST.md) on each actual REAPER target before a full platform release. Browser tests do not replace real-project Undo/redo, file/clipboard and UI acceptance.

Useful commands:

```sh
ctest --test-dir build -C Release --output-on-failure
python tests/plugin_smoke.py --dll build/Release/reaper_reawebapi-x64.dll --webview --runtime
python tests/plugin_smoke.py --dll build/Release/reaper_reawebapi-x64.dll --webview --dev
# Linux/X11; omit the environment override when the default GPU renderer works.
WEBKIT_DISABLE_DMABUF_RENDERER=1 python3 tests/linux_web_runtime.py
python3 tests/linux_webkit_smoke.py
```
