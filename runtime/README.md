# ReaWebAPI SDK

**English** | [简体中文](README.zh-CN.md)

Download `ReaWebAPI-SDK-v<version>.zip` from [Releases](https://github.com/zaibuyidao/ReaWebAPI/releases), or use this directory in the source tree. The SDK is independent of CPU architecture. Install the matching native extension separately.

| File | Use |
| --- | --- |
| `reaper.d.ts` | Editor entry point for the global `reaper`, host methods, handles, events and diagnostics |
| `reaper-api.generated.d.ts` | All 730 REAPER methods, Promise result types, return labels and official links |
| `runtime-api.d.ts` | v0.1.8 Runtime namespace types / 命名空间类型 |
| `runtime-demo/` | Runtime Studio: audio, events, lifecycle, windows and theme / 音频与宿主功能示例 |
| `app-manifest.schema.json` | Minimal App manifest schema / 最小应用描述 |
| `starter/` | Runnable Lua + HTML/CSS/JavaScript template, including `checkJs` configuration |
| `modern/` | Vite/TypeScript template with dev server, IIFE build, resource and Worker examples |
| `web-runtime/` | Unbundled Web Runtime v1 conformance App: modules, local fetch, storage, Canvas, files and Workers |
| `reaper.js`, `reaper-api.generated.js` | Bridge sources used by the extension build. Do not add script tags for these files |

This SDK requires ReaWebAPI 0.1.7 or newer. Keep all three declaration files together. `reaper.d.ts` references the generated mirror and Runtime declarations automatically. They are ambient declarations, not an npm module and not JavaScript to execute.

1. Copy the entire SDK directory to your development workspace.
2. Load `starter/Open.lua` in REAPER's Action List and run it.
3. Edit the four runtime files inside `starter/`. Reopen the window after changes.
4. Open that folder in an editor with TypeScript support. `jsconfig.json` checks JavaScript against the adjacent declarations without emitting files.

For an existing project, include `reaper.d.ts` in `jsconfig.json` or `tsconfig.json`. In the simplest case add `/// <reference path="./SDK/reaper.d.ts" />` to a JavaScript or TypeScript entry file. Compile TypeScript to browser JavaScript before running it.

[Developer guide](../docs/development.md) · [API reference](../docs/api-reference.md) · [Host API](../docs/host-api.md)

The standalone ZIP also contains the machine-readable catalogue and binding manifest under `SDK/api/`, plus a version/revision record and checksums. These files are development references. They do not register APIs and are not required by an installed tool.

[v0.1.8 Runtime API](../docs/runtime-api.md) · [Runtime API 中文](../docs/runtime-api.zh-CN.md)

JavaScript uses `reaper.window.open(path)` and `reaper.lifecycle.ready`, with no flat Runtime aliases. Lua bootstrap remains `reaper.ReaWeb_Open(path)`; these native Lua functions are separate from the browser SDK. The 730 standard REAPER mirror names are unchanged.

[Runtime API 完整清单 / Complete inventory](../docs/runtime-api-inventory.md) — 13 implemented namespaces, 67 methods, 1 Promise property.
