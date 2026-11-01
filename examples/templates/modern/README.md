# TypeScript starter

**English** | [简体中文](README.zh-CN.md)

A Vite and TypeScript template for the current ReaWebAPI release. Development requires Node.js 22.12+.

In the source repository, this template reads declarations from `../../../runtime/`. In the packaged SDK, keep this directory beside the declarations; its packaged type configuration uses `../`. Run `npm ci` and `npm run dev`, then launch `reawebapi-modern-dev.lua` in REAPER. For a production build, run `npm run build` and launch `reawebapi-modern.lua`. Distribute `reawebapi-modern.lua` together with `dist/`.

Set a unique `id` in `public/app.json` when creating an App from this template. Vite copies it into `dist/` for the production origin.

The example covers Mirror-aware Undo batches, file operations, clipboard, external links, transport events, resources and a Worker. The gain control edits the selected track and returns its name and volume through the Builder. Read transport batches playback position, state and tempo in one RPC without requiring a track selection. Save writes `snapshot.json` in the App's file base.

See the [Web runtime guide](../../../docs/frontend.md) for resource paths and development configuration.
