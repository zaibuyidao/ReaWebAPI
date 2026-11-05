-- @noindex
-- @description ReaWebAPI: Close Overlay
local id = tonumber(reaper.GetExtState('ReaWebAPI.OverlayDemo', 'window'))
if id and reaper.APIExists('ReaWeb_Close') then reaper.ReaWeb_Close(id) end
