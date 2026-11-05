# Overlay

Requires ReaWebAPI v0.3.9.24. Run `reawebapi-overlay.lua` from REAPER. Register `restore-interaction.lua` and `close-overlay.lua` as actions before selecting full pass-through.

The frameless transparent window provides a control panel and a separate transparent button. Regions are submitted from `getBoundingClientRect()` after layout, resize and scroll changes. Empty space forwards native mouse input in Regions mode. The range control can be dragged outside its region. Normal mode restores input across the client area. Pass through requires the external Lua recovery action.

Test REAPER global shortcuts with keyboard capture disabled, Space with DAW routing enabled, and text/IME input in the text field. Mouse policy does not change keyboard policy. See [platform requirements and errors](../../../docs/window-overlay.md).
