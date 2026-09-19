# v0.1.8 validation — 2026-09-20

| Check | Result |
| --- | --- |
| Windows x64 / MSVC | Release extension built successfully. All 10 CTest suites passed. |
| Native event synchronization | Selection callbacks trigger a scan on the next main-thread tick. Same-count changes, deselect-all, interrupted large scans, silent-edit fallback and project invalidation passed. |
| Demo state synchronization | Four JavaScript tests passed, covering name/Pan rendering before color conversion, stale-response rejection, continuous updates and Runtime Studio response ordering. |
| WebView2 integration | API Workbench, Starter and Runtime Studio passed in an isolated mock REAPER host with the native extension. Checks include API calls, track display, docking, color editing, diagnostics and cleanup. |
| TypeScript / Vite | SDK type contracts and the modern template production build passed. |
| API / SDK / release | All 730 Mirror bindings verified. Documentation links, SDK checksums, release assembly and LGPL notice inclusion passed. |
| Windows package | Installation staging and platform packaging passed with 82 checksummed files. |

Host callbacks record atomic revisions. REAPER state reads remain on the main thread, and large selections are scanned within the scheduler budget. Event delivery is asynchronous. End-to-end latency in an actual REAPER session depends on host load and the platform WebView.

For native REAPER acceptance, use the [smoke checklist](SMOKE_TEST.md).
