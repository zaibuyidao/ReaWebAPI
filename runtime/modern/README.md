# Modern TypeScript starter

**English** | [简体中文](README.zh-CN.md)

Requires ReaWebAPI v0.1.6 and Node.js 22.12+ (Node 24 recommended for building).

1. Keep this folder beside the SDK declaration files.
2. Run `npm ci`, then `npm run dev` here. Run `OpenDev.lua` in REAPER.
3. Run `npm run build`, then `Open.lua` for the production local App.
4. Ship `Open.lua` and `dist/` together. End users do not need Node.js.

The example demonstrates a selected-track Undo batch, asynchronous files, clipboard, external links, transport events, resource loading and an inline Worker. The gain button edits the selected track; saving explicitly replaces snapshot.json in the page's file base. See the [resource contract](../../docs/frontend.md) for dev/production path differences and WebView constraints.
