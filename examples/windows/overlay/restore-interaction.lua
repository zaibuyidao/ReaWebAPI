-- @noindex
-- Restore Overlay Interaction
local id = tonumber(reaper.GetExtState('ReaWebAPI.OverlayDemo', 'window'))
if not id or not reaper.APIExists('ReaWeb_SetMouseMode') then return end
if not reaper.ReaWeb_SetMouseMode(id, 'normal') then
  reaper.MB(reaper.ReaWeb_GetLastError(), 'ReaWebAPI', 0)
end
