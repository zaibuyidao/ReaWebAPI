# ReaWebAPI developer documentation

**English** | [简体中文](README.zh-CN.md)

Build local HTML/CSS/JavaScript tools that run inside REAPER. ReaWebAPI supplies the dockable WebView and an asynchronous `reaper` object. A small Lua action opens your page.

| Start here | Content |
| --- | --- |
| [Developer guide](development.md) | Project setup, editor types, calling conventions, resources, Undo, debugging and distribution |
| [REAPER API reference](api-reference.md) | All 730 versioned JavaScript signatures, input types, return order and official documentation links |
| [Host API reference](host-api.md) | Windows, docking, events, batches, diagnostics, errors and Lua entry points |
| [SDK](../runtime/README.md) | Files to use, minimal working starter and standalone download |

The catalogue targets REAPER 7.80. Older supported hosts expose the APIs they actually provide. Check capabilities before relying on a particular function. Lua language builtins, `gfx`, `defer`, SWS and other third-party extension APIs are outside this catalogue.

In an installation or SDK ZIP, the sibling SDK directory is named `SDK/` instead of the repository's `runtime/`. Package links are adjusted automatically.

The large bundled Demo is an API workbench. The SDK starter is the smaller starting point for a new tool. The extension injects its own bridge, so pages must not load the SDK's `reaper.js` or generated method list themselves.

For extension maintainers, see the repository's [API Sync guide](https://github.com/zaibuyidao/ReaWebAPI/tree/main/api). API reference and editor declarations are generated together. `python -m tools.api_sync verify` rejects stale generated files before a build.

- [Frontend / 前端资源约定](frontend.md)
- [前端资源约定（中文）](frontend.zh-CN.md)
- [v0.1.7 release notes / 发布说明](release-notes.md)

[v0.1.7 Runtime namespaces](runtime-api.md) · [v0.1.7 命名空间接口](runtime-api.zh-CN.md)

[Runtime API 完整清单 / Complete inventory](runtime-api-inventory.md) — 13 implemented namespaces, 67 methods, 1 Promise property.
