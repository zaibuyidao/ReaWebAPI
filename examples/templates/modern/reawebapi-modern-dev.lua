if not reaper.APIExists("ReaWeb_OpenDev") then error("Install the current ReaWebAPI extension") end
local instance_key = debug.getinfo(1, "S").source
local existing_id = tonumber(reaper.GetExtState("ReaWebAPI.Windows", instance_key))
if existing_id and reaper.ReaWeb_IsOpen(existing_id) then
  reaper.ReaWeb_Focus(existing_id)
  return
end
local id = reaper.ReaWeb_OpenDev("http://localhost:5173/")
if id > 0 then reaper.SetExtState("ReaWebAPI.Windows", instance_key, tostring(id), false) end
if id == 0 then reaper.ShowMessageBox(reaper.ReaWeb_GetLastError(), "ReaWebAPI", 0) end
