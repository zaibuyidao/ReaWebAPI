# API Mirror maintenance

**English** | [简体中文](README.zh-CN.md)

Scope: **all 730 standard C/Lua APIs in REAPER 7.80**. Every entry has a generated typed C++ call, with zero missing bindings. Lua/EEL/Python built-ins, `gfx` and third-party extensions are outside this scope. Their 195 documentation anchors remain in `excludedBuiltins`.

## Responsibilities

| Module | Responsibility |
| --- | --- |
| `tools/api_sync/` | Parse, normalize and compare official definitions, write schema and SDK data without changing native code, binding reviews or Git |
| `tools/native_bindings.py` | Lower Lua/C arguments into typed C++ calls and explicitly accept reviewed binding contracts |
| `src/native.cpp`, `src/native_call.hpp` | Marshalling, buffers, typed handles and resource lifetimes |
| `src/core.cpp`, `src/runtime.cpp` | Main-thread dispatch, document isolation, errors and host facilities |

`api/reaper_api.json` retains signatures, normalized arguments/results, categories and source SHA-256. Each `signatureHash` covers C and Lua contracts. `catalogueHash` covers the complete schema. Documentation is linked and hashed rather than copied. `schemaVersion` versions the data structure, `generatorVersion` the normalization algorithm.

`api/bindings.json` records accepted reviews. `runtime/reaper-api.generated.js` and `.d.ts` derive from this manifest. Generated C++ stays in the build directory, avoiding redundant committed implementation files.

## Routine commands

Python 3.10+, standard library only. From the repository root:

```sh
python -m tools.api_sync check --require-complete
python -m tools.api_sync report
python -m tools.api_sync verify
```

`check` downloads and compares the official help without writes. Exit codes: 0 unchanged, 1 differences/review needed, 2 invalid input. `--require-complete` also requires complete bindings. `report` is offline by default, `--json` emits structured results and `--full` includes complete differences. CMake and CI run the offline `verify` gate.

`--offline` uses committed definitions. `--source FILE_OR_HTTPS_URL` supports official HTML, JSON or Markdown exchange formats. `--sha256 HEX` pins input bytes. Put `--root PATH` before the subcommand to maintain another checkout. API removal requires explicit `--allow-removals`, and a downgrade also needs `--allow-downgrade`.

## Updating upstream APIs

1. Run `check --full` and review additions, removals and signature changes.
2. Run `update` to write definitions and append `api/changelog/<version>.md`. Accepted bindings are unchanged.
3. Review lowering rules in `tools/native_bindings.py`, implementing new types and special conventions. Update the pinned SDK revision in CMake to match the definitions.
4. After review, explicitly regenerate the accepted manifest and SDK:

```sh
python tools/native_bindings.py --manifest
python -m tools.api_sync update --offline
python -m tools.api_sync verify
```

5. Build platform targets, verify new argument/result/lifetime behaviour, review the diff and commit.

`--manifest` accepts current signature fingerprints. Run it only after review. Ordinary builds never run it. Definition synchronization and native binding acceptance remain separate steps.

Every C argument and Lua input/output needs exactly one mapping. Types, lengths, optional arguments and result order are checked. Unrecognized types or incomplete mappings stop the build. Each generated function uses `static_assert` to compare its function pointer type with the pinned official `reaper_plugin_functions.h`. Signature drift, SDK mismatches, missing registrations and stale SDK output all fail the build. Documentation-only changes do not invalidate signature reviews.

## JavaScript contract

- Arguments and results follow Lua order. One result resolves to a scalar, multiple results to an array, void to `undefined`. Optional arguments can be omitted or passed as `null`, preserving positional holes.
- `GetTrackName` returns `[ok, name]`, replacing the early string-only result. Project and index arguments now follow the explicit official signature.
- Native objects use typed opaque handles. Projects accept `0`, `null` or a handle from `EnumProjects`, including other open projects. Handles cannot be shared across pages or fabricated from addresses.
- Project objects use `ValidatePtr2` and applicable GUID checks. Markers are looked up by GUID, windows use platform validation. Creation/destruction updates the registry. Closing or reloading a page releases resources it created and has not transferred.
- A `PCM_source` transferred to a take belongs to the project. Replaced sources can be destroyed explicitly. Destroying a source still referenced by a project is rejected. Audio accessors and joysticks support explicit release.
- Binary inputs accept `Uint8Array`, or UTF-8 strings for text. Binary outputs return `Uint8Array`, preserving NUL and high bytes. This applies to MIDI, PCM sink configuration and hardware MIDI messages.
- The three `reaper.array` APIs accept `Float64Array` or `number[]`. Capacity is checked and values are written back before the Promise resolves. Float64 arrays travel as little-endian IEEE 754 bytes, preserving special floating-point values. Keep the array length unchanged while a call is pending.
- GUIDs use `{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}` strings. RECTs expand into the Lua coordinate arguments. Nullable native strings and GUIDs resolve to `null`.
- Integers are checked against their C ABI range. `size_t` also respects JavaScript's safe integer limit. Ordinary scalar floating-point arguments must be finite.

`NeedBig` buffers grow through REAPER's `realloc_cmd_register_buf` and are released after copying the results. Fixed buffers default to 64 KiB. `ReaWeb_SetBufferSize(bytes)` adjusts them from 4 KiB to 16 MiB. MIDI event readers can grow and retry using the size reported by REAPER. Individual strings/binary results are capped at 16 MiB, sample arrays at 1,048,576 doubles, and JSON messages at 64 MiB. Oversized results fail explicitly instead of silently truncating.

`ReaWeb_Batch` retains prevalidators for the nine track APIs listed in the SDK. Other APIs use ordinary async calls. The queue deadline applies until native execution starts, not while waiting for a dialog or render to finish. If manually opening Undo/UI refresh scopes across asynchronous calls, pair their end calls in `try/finally`.

## Availability and verification

`ReaWeb_GetCapabilities().api.implemented` counts bindings compiled into the extension. `available` counts functions resolved in the running REAPER. `unavailable` lists host functions that are missing and will reject with `API_UNAVAILABLE`. Older REAPER versions keep working for their existing APIs. All functions in this catalogue require REAPER 7.80 or newer.

Compilation verifies ABI types and complete registration. Development checks include 730 harmless mocked native calls, JS binary/array tests and the demo in real WebViews. Mocked calls do not mean every mutating API has been exercised in a real REAPER project. Releases still need real REAPER regression testing on each platform.

The demo's **Run read-only checks** covers ten paths without project edits. Accessors are released in `finally`. Empty projects show skips. Color, pan and cursor writes require their respective button clicks.

The extension embeds the schema projection, native calls and JavaScript methods. No runtime JSON installation or network access is needed. ReaPack remains seven native files under `extension/` and the root `ReaWebAPI.ext`.

## Exchange inputs

JSON is a complete snapshot: `reaperVersion` and `functions` indexed by API name, each containing `signatures.c` and `signatures.lua`. Markdown uses `<!-- reaper-version: 7.80 -->`, `## FunctionName` and matching `c`/`lua` fences. Add `--source-version` for an unversioned source. Partial snapshots trigger the removal guard rather than merging as patches.
