# Validation record — 2026-09-19

| Target | Result |
| --- | --- |
| Windows x64 / MSVC 19.44 | Release DLL compiled; core, runtime, JavaScript bridge and plugin ABI tests passed |
| Windows / WebView2 153 | Actual browser round-trip against a mock REAPER host passed: Unicode track names, Unicode/space/#/% paths, two windows, early close, shared UDF |
| Linux x86_64 / Ubuntu 24.04 / GCC 13.3 / WebKitGTK 2.52 | Release `.so` compiled; core, runtime and native plugin ABI tests passed |
| macOS arm64 / x86_64 | Source and GitHub Actions targets provided; not compiled locally |
| Linux aarch64 | GitHub Actions target provided; not compiled locally |

The JavaScript suite includes six tests covering both transport shapes. Native ABI tests load the actual compiled extension, resolve its exported entry point and check REAPER API registrations and unload. They use a mock host, not a running REAPER installation.

Windows installation ZIP contents and SHA-256 checksums were verified. Workflow YAML parsed successfully and all referenced action major tags were checked to exist. The remote GitHub Actions workflow has not been executed from this workspace.

Real REAPER UI acceptance remains the checklist in [SMOKE_TEST.md](SMOKE_TEST.md). Linux browser interaction and macOS Web Inspector still need platform-host validation.
