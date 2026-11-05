-- @noindex
-- @description ReaWebAPI: Open Overlay
if not reaper.APIExists('ReaWeb_SetMouseMode') then
  reaper.MB('Install ReaWebAPI v0.3.9.24 or later and restart REAPER.', 'ReaWebAPI', 0)
  return
end
local directory = debug.getinfo(1, 'S').source:sub(2):match('^(.*[/\\])')
local id = reaper.ReaWeb_OpenEx(directory .. 'index.html',
  '{"instanceKey":"reawebapi-overlay-demo","frameless":true,"transparent":true,"layer":"overlay","mouseMode":"regions"}')
if id == 0 then reaper.MB(reaper.ReaWeb_GetLastError(), 'ReaWebAPI', 0)
else reaper.SetExtState('ReaWebAPI.OverlayDemo', 'window', tostring(id), false) end
