# TypeScript starter

**English** | [简体中文](README.zh-CN.md)

A Vite and TypeScript template for ReaWebAPI 0.1.7+. Development requires Node.js 22.12+.

Keep this directory beside the SDK declarations. Run `npm ci` and `npm run dev`, then launch `OpenDev.lua` in REAPER. For a production build, run `npm run build` and launch `Open.lua`. Distribute `Open.lua` together with `dist/`.

The example covers track Undo batches, file operations, clipboard, external links, transport events, resources and a Worker. The gain control edits the selected track, and Save writes `snapshot.json` in the App's file base.

See the [Web runtime guide](../../docs/frontend.md) for resource paths and development configuration.
