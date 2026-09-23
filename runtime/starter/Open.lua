-- @description ReaWebAPI: Starter
-- @about Opens the local starter page. Rename this action for your tool.

if not reaper.APIExists("ReaWeb_Open") then
  reaper.MB("Install the current ReaWebAPI extension in UserPlugins and restart REAPER.", "ReaWebAPI", 0)
  return
end

local source = debug.getinfo(1, "S").source:sub(2)
local directory = source:match("^(.*[/\\])")
local instanceKey = debug.getinfo(1, "S").source
local id = reaper.ReaWeb_Open(directory .. "index.html", instanceKey)
if id == 0 then
  reaper.MB(reaper.ReaWeb_GetLastError(), "ReaWebAPI", 0)
end
