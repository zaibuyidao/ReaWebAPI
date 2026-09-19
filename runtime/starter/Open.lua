-- @description ReaWebAPI: Starter
-- @about Opens the local starter page. Rename this action for your tool.

if not reaper.APIExists("ReaWebOpen") then
  reaper.MB("Install ReaWebAPI in UserPlugins and restart REAPER.", "ReaWebAPI", 0)
  return
end

local source = debug.getinfo(1, "S").source:sub(2)
local directory = source:match("^(.*[/\\])")
local id = reaper.ReaWebOpen(directory .. "index.html")
if id == 0 then
  reaper.MB(reaper.ReaWeb_GetLastError(), "ReaWebAPI", 0)
end
