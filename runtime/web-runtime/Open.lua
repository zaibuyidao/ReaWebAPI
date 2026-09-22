local source = debug.getinfo(1, 'S').source:sub(2)
local folder = source:match('^(.*[/\\])')
if not reaper.APIExists('ReaWeb_Open') then
  reaper.MB('Install the current ReaWebAPI extension.', 'Web Runtime checks', 0)
  return
end
local instance_key = debug.getinfo(1, "S").source
local existing_id = tonumber(reaper.GetExtState("ReaWebAPI.Windows", instance_key))
if existing_id and reaper.ReaWeb_IsOpen(existing_id) then
  reaper.ReaWeb_Focus(existing_id)
  return
end
local id = reaper.ReaWeb_Open(folder .. 'index.html')
if id > 0 then reaper.SetExtState("ReaWebAPI.Windows", instance_key, tostring(id), false) end
if id == 0 then reaper.MB(reaper.ReaWeb_GetLastError(), 'Web Runtime checks', 0) end
