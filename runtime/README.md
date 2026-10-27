# ReaWebAPI SDK

**English** | [简体中文](README.zh-CN.md)

TypeScript declarations, templates and examples for the current ReaWebAPI release. Download the SDK from [Releases](https://github.com/zaibuyidao/ReaWebAPI/releases) or use this directory with an installed extension.

| Entry | Purpose |
| --- | --- |
| `reaper.d.ts` | Type declaration entry point, referencing the Mirror and Runtime declarations |
| `reaper-api.generated.d.ts` | 730 REAPER API signatures and result types |
| `runtime-api.d.ts` | Runtime namespaces and data types |
| `jsfx-webui.mjs` | [JSFX Web UI](../docs/jsfx-webui.md): `params`, shared `editor` sizing, Promise-based coalesced writes, grouped gestures, DSP streams, state, commands and binary resources. |
| `browser-files.mjs` | Opt-in Blob/data URL export through native save dialogs, with `browser-files.d.mts` declarations. See [browser media](../docs/browser-media.md). |
| [Native Service Demo](../web/native-service/README.md) | Native events, services and Lua coexistence. Packaged SDK also includes `native/reaweb_service.h` and `native/test_extension.cpp`. See [integration](../docs/native-services.md). |
| `starter/` | Lua, HTML and JavaScript template with editor type checking |
| `modern/` | Vite and TypeScript template |
| `runtime-demo/` | Runtime Studio: windows, events, audio and App services |
| `web-runtime/` | Modules, resources, storage and browser capability checks |
| `app-manifest.schema.json` | App metadata schema |

Copy the SDK to your workspace and run `starter/Open.lua` from REAPER's Action List. For an existing project, include `reaper.d.ts` in `jsconfig.json` or `tsconfig.json` and keep the three declaration files together.

Customize the starter's native window name in `starter/index.html` using `<title>`. Dynamic titles and explicit overrides are described under [window titles](../docs/runtime-api.md#window-titles).

The extension injects the browser runtime. Lua opens the page with `reaper.ReaWeb_Open(path, instanceKey)`. Pass `debug.getinfo(1, "S").source` as `instanceKey` to reuse the launcher's window. Omitting the key creates a new window and requires `app.json.id`. Otherwise the launcher filename defines the fallback App ID. Production pages use `reaweb://<appId>/...`. Assign a unique ID when copying a template to create another App. JavaScript uses the Mirror and Runtime APIs after `reaper.lifecycle.ready` resolves.

`reaper.transaction.batch(b => { ... })` collects reviewed Mirror calls with automatic result references and tuple destructuring. Return a reference, object or array to select typed results. The callback is synchronous. The [batch contract](../docs/host-api.md#batches-and-continuous-controls) also documents the existing call-array form.

[Developer guide](../docs/development.md) · [REAPER API](../docs/api-reference.md) · [Runtime API](../docs/runtime-api.md) · [API inventory](../docs/runtime-api-inventory.md) · [DevTools](../docs/devtools.md)

Pure JavaScript apps use the REAPER Mirror and Runtime APIs. For Lua-owned project logic, use the [Lua backend + WebView example](../web/lua-backend/README.md). Platform packages install it under `Scripts/ReaWebAPI/Example/lua-backend`, and the SDK includes `lua-backend/`.

EEL2 backends use the same message bridge. The SDK includes a [minimal example](../web/eel-backend/README.md) and a [bidirectional track mixer](../web/eel-mixer/README.md), installed under `eel-backend/` and `eel-mixer/`.

[Native Stream Demo](../web/native-stream/README.md) · [Native Stream ABI and platform contracts](../docs/native-streams.md). The SDK includes `native/reaweb_stream.h`, `native/reaweb_tasks.h` and `native/stream_extension.cpp`.

[Hidden startup / 隐藏启动示例](hidden-start/README.md)

[Frameless Window](../web/frameless/README.md) provides resizable and fixed-size Lua launchers with native HTML drag regions. The packaged SDK includes this example in `frameless/`.

[Transparent Window](../web/transparent/README.md) demonstrates desktop Alpha, translucent panels, rounded corners and shadows. It is included in the SDK as `transparent/`.

The JSFX helper shares its initialization path across Windows, macOS and Linux. After the capability handshake, it registers subscriptions concurrently, then reads initial size and parameters concurrently. Initial `STALE_SESSION` errors trigger a capability refresh and at most two snapshot retries. Writes are not retried. Distribute the updated `jsfx-webui.mjs` with your UI. See [connection semantics](../docs/jsfx-webui.md#parameter-service) and [startup timing](../docs/devtools.md#startup-timing).
