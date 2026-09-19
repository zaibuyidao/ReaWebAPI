# Web Runtime checks

**English** | [简体中文](README.zh-CN.md)

Requires ReaWebAPI v0.1.6. Run `Open.lua` in REAPER. No build, npm or bridge import is needed. Keep the entire directory together.

The page imports ordinary modules, fetches local JSON and checks required/optional browser capabilities. Reopen to see localStorage, cookie and IndexedDB visit counters increase. Copy this folder to a different directory to check storage isolation. Drag text or a real file onto the drop area for manual OS integration acceptance; automatic checks dispatch a DOM event only.

The checks write only their own browser storage keys/database. They read the REAPER version/capabilities without editing projects. The WebSocket constructor check is only availability; automated native-host tests add local HTTP/WebSocket servers for real round-trips. No external service is contacted by this example.

See the [Web Runtime contract](../../docs/frontend.md) for scope, storage paths, migration and platform requirements. Leave the page visible while checking animation frames.
