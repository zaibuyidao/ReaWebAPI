# Windows isolation acceptance — v0.3.9.43

Environment: Windows, REAPER 7.85/x64, WebView2 157.0.4341.46. Native browser tests use the production extension with a dedicated mock host profile. Full-entry checks also run in an isolated REAPER instance.

| Check | Result |
| --- | --- |
| Isolation, URL/Blob Workers, shared WASM, Atomics | Passed |
| Required-mode failure before App execution, CSP, CORS/CORP | Passed |
| App origin/storage persistence, reload, concurrent windows | Passed |
| DOSBox Pure threaded input, pause/resume, state restoration | Passed with Windows keyboard messages and deterministic screen/state assertions |
| FCEUmm single/threaded builds | Passed the same behavioral checks, memory sharing matches the selected build |
| PPSSPP 64 MiB RAM, controller input, pause/resume, state restoration | Passed with the local arena correction described below |
| PSP WebGL2 and Web Audio | Actual WebGL2 context and nonzero audio output buffers verified |
| DOS and corrected PSP close/reopen | Each passed 20 reopens, Workers return to 0, no residual PSP test processes |
| Continuous 30-minute DOS run | Passed (1800.1 s), 4 Workers, no residual test processes |
| ReaEmulator full entry in REAPER | Threaded NES and corrected PSP passed through the platform selector |

The original EmulatorJS 4.2.3 PPSSPP WASM reserves `0x0A000000` bytes for its PSP address arena. RAM begins at `0x08000000`, leaving 32 MiB. The 64 MiB path needs `0x0C000000` bytes. The original `cube.elf` failure also reproduces in standalone Edge. Its allocator trap occurs at WASM offset `0x184a5` after heap corruption.

The ReaEmulator correction changes the allocation immediate at `0xa19564` from `d0` to `e0`. The remaining WASM bytes, metadata and license members are unchanged. Two JavaScript loader corrections handle interrupted Web Audio recovery on user input and rejected optional screen wake-lock requests. The adapter includes a source-equivalent memory diff, exact JavaScript replacements, a guarded reproducible patcher and original/patched hashes. These are local core corrections. ReaWebAPI contains no PSP-specific workaround. DOSBox Pure and FCEUmm remain unmodified.

PSP rendering uses the original core's software fallback and presents through WebGL2. This does not establish hardware-accelerated PSP rendering. Audio checks establish running Web Audio contexts and nonzero submitted samples, not a listening test. Commercial-game compatibility, physical controllers and large-file export are outside this acceptance. See the separate [WebKit acceptance record](webkit-isolation-validation.md) for macOS/Linux.

## Reproduction

Use a new output directory for each run. Supply a ReaEmulator directory and legally obtained PSP fixtures.

```powershell
python tests/plugin_smoke.py --dll build/Release/reaper_reawebapi-x64.dll --webview --isolation --resource-root build/new-isolation
python tests/plugin_smoke.py --dll build/Release/reaper_reawebapi-x64.dll --webview --isolation-emulator PATH_TO_REAEMULATOR --isolation-core ppsspp --isolation-psp-program PATH_TO_CONTROLLER_PBP --isolation-psp-scenario controller --resource-root build/new-psp-input
node tests/isolation_soak.cjs build/Release/reaper_reawebapi-x64.dll PATH_TO_REAEMULATOR build/new-soak 1800
node tests/isolation_soak.cjs build/Release/reaper_reawebapi-x64.dll PATH_TO_REAEMULATOR build/new-reopen 0 20
```

The monitor records CDP Worker targets, Windows process memory and exit cleanup. Reopen samples explicitly record temporary debugging-connection gaps while browser processes restart. A gap is not counted as zero Workers.

Mean process private memory after warmup changed from 492.6 to 489.6 MiB. All recorded native close calls were below the 15.625 ms resolution of the clock used in this run. Asynchronous process exit was checked separately.
