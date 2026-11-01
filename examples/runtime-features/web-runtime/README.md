# Web runtime checks

**English** | [简体中文](README.zh-CN.md)

A browser capability check App for the current ReaWebAPI release. Keep the directory together and run `reawebapi-web-runtime.lua` in REAPER.

The page checks modules, local JSON loading, storage, Canvas and Workers. Reopen it to verify persistent visit counters. Copy the App to another directory and assign it a different `app.json.id` to check storage isolation. The same ID in two existing roots is rejected. Cookies follow the engine's custom-scheme support. Drag native files or text into the drop area to check system integration. Keep the window visible during animation checks.

The results distinguish required and optional browser capabilities. Platform behavior and storage paths are documented in the [Web runtime guide](../../../docs/frontend.md).
