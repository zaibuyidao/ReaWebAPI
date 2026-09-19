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
