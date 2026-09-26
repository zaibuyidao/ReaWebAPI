# v0.3.6.3 validation — 2026-09-26

| Check | Result |
| --- | --- |
| Windows x64 / MSVC | Release extension and test extension built. All 16 CTest suites passed. |
| Linux x86_64 / GCC, WSL Ubuntu | Release extension, WebKit helper and test extension built. All 15 available CTest suites passed. Lua CLI is unavailable in this environment. |
| macOS ARM64 / AppleClang 17 | Release extension and test extension built on Apple M4, macOS 26.0.1. All 16 CTest suites passed. Real REAPER 7.74 checks passed. |
| macOS Intel / AppleClang 17 | Release extension and test extension cross-compiled successfully for x86_64. Runtime validation remains with the existing Intel CI runner. This Mac has no Rosetta runtime. |
| Linux ARM64 | Covered by the existing CI target. Not executed locally. |
| JavaScript / TypeScript / Mirror | 43 bridge tests passed, including both WebView2 and WebKit transport adapters. Strict SDK type checks passed. All 730 Mirror definitions/bindings verified. |
| Native Service | Invoke/send/events, missing/duplicate registration, worker completion, timeout, repeated disposal/unregister, unregister while pending, re-registration, queue limits and close/reload cleanup passed. |
| Native Monitor | Full ordered selection comparison, same-first replacements, empty selection, shared subscription counts, stop/restart, incremental scans and track/transport/project/timeline state comparisons passed. |
| Real REAPER / Windows and macOS ARM64 | `native_communication_smoke.py` passed with an immediately returning Lua launcher and with a Lua defer echo backend. Native states and third-party services remained active. Real checks include play/pause/stop/rate/repeat, project open/close/switch/save path/dirty, marker/region metadata, time/loop selection, pending unload rejection and dock/undock. |
| Lua Backend regression / Windows and macOS ARM64 | `message_bridge_smoke.py` passed in isolated REAPER processes: real Lua ABI, Unicode, JSON, FIFO, independent windows, reload, closed-window rejection and the existing Lua Backend UI volume update. |
| SDK / release contracts | 41 Python tests passed. SDK contents, documentation links, checksums, platform metadata and ReaPack ZIP layout/changelog verified. Release assembly tests use synthetic binaries, never distributed. |

The new registry, router and monitor code is shared by all three platforms. Host callbacks publish atomic revisions. REAPER state reads and service callbacks execute on the main thread. Worker completions/events are queued for native dispatch. Ordinary state events are bounded notifications, not a high-frequency stream.

Local artifacts do not constitute a published release. The complete five-platform ReaPack archive is assembled from the CI builds. Its descriptor is generated from this version's [release notes](release-notes.md), excluding earlier changes. No Git commit or release publication is part of this validation.

For remaining platform host acceptance, use the [smoke checklist](SMOKE_TEST.md). Native Stream is outside Part 1.
