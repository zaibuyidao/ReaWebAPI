# ReaWebAPI SDK

**English** | [简体中文](README.zh-CN.md)

TypeScript declarations, templates and examples for ReaWebAPI 0.1.7+. Download the SDK from [Releases](https://github.com/zaibuyidao/ReaWebAPI/releases) or use this directory with an installed extension.

| Entry | Purpose |
| --- | --- |
| `reaper.d.ts` | Type declaration entry point, referencing the Mirror and Runtime declarations |
| `reaper-api.generated.d.ts` | 730 REAPER API signatures and result types |
| `runtime-api.d.ts` | Runtime namespaces and data types |
| `starter/` | Lua, HTML and JavaScript template with editor type checking |
| `modern/` | Vite and TypeScript template |
| `runtime-demo/` | Runtime Studio: windows, events, audio and App services |
| `web-runtime/` | Modules, resources, storage and browser capability checks |
| `app-manifest.schema.json` | App metadata schema |

Copy the SDK to your workspace and run `starter/Open.lua` from REAPER's Action List. For an existing project, include `reaper.d.ts` in `jsconfig.json` or `tsconfig.json` and keep the three declaration files together.

The extension injects the browser runtime. Lua opens the page with `reaper.ReaWeb_Open(path)`. JavaScript uses the Mirror and Runtime APIs after `reaper.lifecycle.ready` resolves.

[Developer guide](../docs/development.md) · [REAPER API](../docs/api-reference.md) · [Runtime API](../docs/runtime-api.md) · [API inventory](../docs/runtime-api-inventory.md)
