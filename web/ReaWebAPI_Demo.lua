-- @description ReaWebAPI Demo
-- @noindex
-- @about Opens the bundled JavaScript REAPER API demo.

if not reaper.APIExists("ReaWeb_Open") then
  reaper.MB("Install the current ReaWebAPI extension in UserPlugins and restart REAPER.", "ReaWebAPI", 0)
  return
end

local source = debug.getinfo(1, "S").source:sub(2)
local directory = source:match("^(.*[/\\])")
local instance_key = debug.getinfo(1, "S").source
local existing_id = tonumber(reaper.GetExtState("ReaWebAPI.Windows", instance_key))
if existing_id and reaper.ReaWeb_IsOpen(existing_id) then
  reaper.ReaWeb_Focus(existing_id)
  return
end
local window = reaper.ReaWeb_Open(directory .. "index.html")
if window > 0 then reaper.SetExtState("ReaWebAPI.Windows", instance_key, tostring(window), false) end
if window == 0 then
  reaper.MB(reaper.ReaWeb_GetLastError(), "ReaWebAPI", 0)
end
