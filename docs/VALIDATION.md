# v0.1.8 source-layout validation — 2026-09-19

| Target | Result |
| --- | --- |
| Windows x64 / MSVC | Release extension build and all 9 CTest suites passed, including the native ABI, Runtime, JavaScript, Python/SDK and resource checks |
| Linux x86_64 / GCC | Release extension and WebKit helper built; all 8 native/Python/resource suites passed; the JS bridge suite was excluded because this WSL cache selects Windows node.exe, and passed on Windows |
| Optional Windows native-drag target | Compiled and linked against the shared `reaweb_runtime` library; interactive gestures were not rerun for this source-only refactor |
| TypeScript / Vite | Strict SDK contract check and modern starter production build passed with package version 0.1.8 |
| Mirror / browser contract | API Sync verified all 730 definitions and reviewed bindings; the Mirror catalogue, binding manifest, browser bridge and all declaration files are unchanged from the preceding revision |
| Installation / packaging | Windows CMake installation and local v0.1.8 platform package succeeded, including SDK/source-layout documentation and 79 checksummed files; standalone SDK packaging/link/checksum checks passed in the Python suite |
| Source references | Build targets, generated native includes, ABI generator, API Sync fixtures, batch checks and documentation use the new paths; the old flat source references were checked for leftovers |
| macOS / ARM targets | CMake paths and qualified includes updated; no local macOS or ARM build/runtime result is claimed |

The five source groups and build-target boundaries are described in [source layout](source-layout.md). Existing Runtime definitions were accounted for after extraction; host-call routing now delegates managed Undo to `transaction.cpp`. The macOS lossless file-time conversion and its 64/128-bit regression cases are retained.

The local packaging check is under `.cache/v018-package-check/` and uses revision `local-v0.1.8-layout`. No commit, push, tag or remote publication was performed. The release unit test's `github.com/test/repo` output is a mocked fixture. Prior browser/native interaction results below belong to their original versions; they were not rerun as v0.1.8 acceptance.

---

# v0.1.7 validation — 2026-09-19

| Target | Result |
| --- | --- |
| Standard REAPER mirror | 730 definitions/reviewed bindings and all 730 typed native ABI mappings passed; original generated mirror files are unchanged |
| Windows x64 / MSVC | Release build and all 9 CTest suites passed, including 29 JS cases, 22 Python contracts and the new native audio suite |
| Linux x86_64 / Ubuntu 24.04 | Release build and all 8 native/Python/HTTP CTest suites passed; JS VM coverage ran on Windows because the local WSL CMake cache selects Windows node.exe |
| Runtime native coverage | Window geometry/visibility/Docker rejection, lifecycle acknowledgements/wrong tokens/timeouts, async file saving, event invalidation/deltas/save/load, logs and cleanup passed |
| Audio native coverage | Independent PCM-source fixture checked metadata, channel-interleaved min/max data, incremental peak building, cancelled-source cleanup and error paths; native meter validation checked stale/null handles and linear-to-dB values |
| WebView2 / mock REAPER | Native resize/position/show/hide, theme, track meter, asynchronous reload/close file persistence, Unicode, multiple windows and dock/undock passed |
| Runtime Studio / WebView2 | Shipped module App passed initialization, theme application, selected-track/meter display, resize, docking, diagnostics and reload state persistence |
| Web Runtime v1 | All 17 required/exercised optional groups passed in WebView2 and WebKitGTK, including modules, local fetch, both Workers, Canvas/WebGL, HTTP CORS, WebSocket, persistent storage and cross-App isolation |
| WebKitGTK embedding/reload | Large message, two windows, reparenting after parent destruction, preserved page state, fragment navigation without cleanup, two successive host-gated reloads and independent close passed |
| SDK/manifest | Strict TypeScript contract and Vite production build passed; schema/entry checks, SDK links, content and checksum packaging passed |
| Windows source-only CRLF checkout | Actual Git CRLF checkout plus all modified/new v0.1.7 files preserved every source byte and mtime through offline verify/update. Python: 21 passed, one optional cached-HTML test skipped; no .cache dependency |
| macOS arm64/x86_64 and Linux aarch64 | Backends and existing CI matrix updated/preserved; not compiled or run locally |

These are local results, not a completed GitHub Actions run. New native dialog calls reuse the checked standard mirror; interactive dialogs, actual decoder formats/waveforms, theme changes and real-project event/Undo effects still require the per-platform REAPER acceptance in [SMOKE_TEST.md](SMOKE_TEST.md). Audio tests use an independent native fixture rather than a real REAPER decoder. The Linux browser runs used WSLg with WEBKIT_DISABLE_DMABUF_RENDERER=1; the extension does not force that setting.

Browser follow-ups caught and fixed a WebKit hash-navigation cleanup regression. Ordinary Runtime log output is also checked not to overwrite the Lua last-host-error value. Track event GUID formatting matches the uppercase standard mirror representation. The old Windows smoke test also had obsolete no-argument CountTracks and null-string enumeration assertions; these now follow the unchanged standard mirror contract. Linux Web Runtime tests now use unique fixture directories so reused WSL process IDs do not collide with previous reports.

Local Windows/Linux platform bundles and the independent SDK are staged under dist/v0.1.7 with revision local-v0.1.7. No commit, push or remote release was performed. A “Published https://github.com/test/repo/…” line printed by the Python release unit test is a mocked publication fixture, not a network publication.

The new App/system/native-drag checks cover five App getters, shared/reopened identity, separate data directories, optional metadata validation, write probes, platform/architecture, native path validation, native drop ordering without replay, callback removal during pending subscriptions, and drag ownership after source closure.

Windows WebView2/OLE integration uses two disposable real browser windows: Unicode file/text copy round trips, actual native paths and Escape cancellation passed. GTK/WebKit integration uses disposable native/browser windows: Unicode file/text round trips in both directions, copy results, coordinates and gesture rejection passed. These do not claim actual REAPER Arrange import acceptance or macOS runtime validation. The Linux Python test target emits GTK widget-cleanup warnings; the native helper completes the tested transfers.

Runtime Studio now displays App metadata/data path/platform/architecture, offers a data-folder button and native drag sources, and shows incoming drop payloads. Type declarations, bilingual behavior docs and the inventory are synchronized.

JavaScript exposes exactly 730 unchanged Mirror functions and thirteen frozen Runtime namespaces (67 methods plus lifecycle.ready). All thirteen namespaces are implemented and runtime.reservedNamespaces is empty. App metadata/data paths, native file/text drag/drop, platform/architecture/file-manager methods and events.off are implemented; debug.info is removed. Eight existing methods were moved to transaction, clipboard and system, with previous namespace paths removed. No flat ReaWeb_* or root ready aliases remain. Namespace/transport regression tests cover window IDs, host errors, subscriptions, batch/Undo, binary/text file helpers and clipboard services. Lua native bootstrap remains ReaWeb_Open; native protocol command names are private to the bridge. The complete inventory is checked against the actual JavaScript surface.

Useful v0.1.7 commands:

```sh
ctest --test-dir build -C Release --output-on-failure
python tests/plugin_smoke.py --dll build/Release/reaper_reawebapi-x64.dll --webview
python tests/plugin_smoke.py --dll build/Release/reaper_reawebapi-x64.dll --webview --studio
python tests/plugin_smoke.py --dll build/Release/reaper_reawebapi-x64.dll --webview --runtime
WEBKIT_DISABLE_DMABUF_RENDERER=1 python3 tests/linux_web_runtime.py
WEBKIT_DISABLE_DMABUF_RENDERER=1 python3 tests/linux_webkit_smoke.py
python tools/validate_app.py runtime/runtime-demo/app.json
```

---

# v0.1.6 validation — 2026-09-19

| Target | Result |
| --- | --- |
| Standard REAPER 7.80 mirror | 730 definitions, 730 reviewed bindings and all 730 typed native call/return mappings verified |
| Windows x64 / MSVC 19.44 | Release build; 8 CTest suites passed, including 17 JS cases, 19 Python cases and 8 HTTP resource cases (4 pass; 4 require unavailable Windows symlink privilege and are explicitly skipped) |
| WebView2 153 / mock REAPER | Demo with MIDI/audio buffers, modern production template, batch/managed Undo, 100 KB binary files, docking and Vite CSS HMR passed |
| WebView2 Web Runtime v1 | All 9 required groups and 8 exercised optional groups passed; same-App reopening and two independent host processes retained localStorage/cookies/IndexedDB and the same origin; separate App directories started with independent storage |
| Linux x86_64 / Ubuntu 24.04 / GCC 13.3 | Release build; 7 native/Python/HTTP CTest suites passed; JS VM suite ran on Windows because this WSL build cache locates Windows node.exe |
| WebKitGTK 2.52.6 / WSLg | Same 17 Web Runtime check groups passed with `WEBKIT_DISABLE_DMABUF_RENDERER=1`, including nonzero viewport, animation frames, WebGL, HTTP CORS, WebSocket round-trip, both Workers, storage isolation and browser process restart |
| WebKitGTK embedding/transport | Two windows, large 100,000-byte JS request, reload, reparenting after original parent destruction and independent close passed |
| SDK | Strict TypeScript contracts, Vite build, SDK links and UTF-8 file/checksum packaging passed |
| macOS arm64/x86_64, Linux aarch64 | Sources and CI targets provided; not compiled or run locally |

The WSLg default DMA-BUF renderer stalled native animation frames; the documented WebKit environment switch fixed the graphics path. The extension does not force this switch and does not replace requestAnimationFrame. Linux viewport allocation was also corrected for a foreign X11 parent. macOS uses localhost instead of an IP-literal URL for ATS compatibility, but still requires validation against real REAPER on macOS.

HTTP tests exercise actual compiled resource servers (the original Windows run did not create privileged symlinks; the follow-up below adds always-run Windows junction coverage): MIME/Unicode/query paths, HEAD/ranges, directory traversal and symlink escape rejection, Host/Origin restrictions, read-only methods, stable port restoration and port exclusivity. API tests use independent native stubs and a mock REAPER host; they do not prove every API's project side effects.

Physical OS file drops and actual REAPER drag gestures remain manual acceptance cases; automated checks dispatch standard DOM events. External TLS endpoints were not tested; network fixtures verify HTTP CORS and native WebSocket echo. Optional GPU/network capabilities remain platform dependent.

The local Windows/Linux/SDK ZIPs and SHA-256 manifests are generated under `dist/v0.1.6/` with revision `local-v0.1.6-ci-fix`. No project license, custom Lua services or third-party REAPER API registration was added. Third-party C++ dependency notices are preserved. These fixes have been verified locally; a new GitHub Actions run is still needed to confirm the remote result.

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

## v0.1.6 CI failure follow-up

The supplied Windows CI log exposed two independent failures:

- Eleven API-sync tests failed during setup because the fresh checkout had no untracked .cache directory. Tests now allocate their own system temporary directory and register cleanup immediately.
- The HTTP library uses Windows _fullpath, which does not resolve symlinks/junctions. A resource link could serve a file outside the App root. ReaWebAPI now checks the resolved filesystem path itself before serving, including a trailing-slash request's implicit index.html.

The Windows junction regression reproduced 200/206 responses before the fix and now verifies 403 for GET, HEAD and Range, including a sibling directory with an App-prefixed name. Internal junction resources remain readable. Four separate symlink cases run when the platform permits symlink creation; Windows privilege failures are explicit skips, not silent success. Linux passes all four symlink cases and skips only the Windows-specific junction case.

Revalidation: Windows 8/8 CTest suites; Linux 7/7 suites (JS VM coverage on Windows); real WebView2 module/fetch/Unicode/storage/Worker checks passed. An independent source-only checkout with no .cache passed all applicable Python contracts: 18 passed, one optional cached-official-HTML test skipped; no .cache was created. This keeps CI offline and leaves the 730 REAPER API bindings and version 0.1.6 unchanged.

## Windows CRLF checkout follow-up

The next Windows CI run passed the HTTP resource suite but failed the offline no-op assertion: Git had checked generated files out with CRLF, while `update --offline` compared their bytes against LF output and rewrote them. `verify` already accepted both line endings. The previous source-only copy retained local LF bytes and did not reproduce this Git checkout conversion.

The sync writer now treats LF/CRLF differences alone as unchanged, preserving the original bytes and modification times. Actual content drift still triggers regeneration and atomic replacement. The no-op and SDK drift tests explicitly cover both line endings on every platform; snapshot failures identify the changed file instead of constructing a multi-megabyte assertion diff.

Revalidation after this fix:

- Windows: all 8 CTest suites passed, including all 19 Python contracts.
- Linux: all 7 configured CTest suites passed, including the LF/CRLF regressions; JS VM coverage remains on Windows.
- A separate checkout created with `git -c core.autocrlf=true checkout-index`, overlaid with the current fixes, verified CRLF generated files and no .cache. Offline verification and update preserved every source file's bytes and modification time, with `written: []`. All 18 applicable Python contracts passed; only the optional cached-official-HTML test was skipped.

This follow-up changes the sync tool, regression coverage and documentation only. The extension remains version 0.1.6 with 730 REAPER bindings. These are local results; the corrected commit still needs a GitHub Actions run.
