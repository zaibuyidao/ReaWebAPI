-- @description ReaWebAPI: Example
-- @version 0.1.2
-- @about Opens the bundled JavaScript REAPER API demo.

if not reaper.APIExists("ReaWebOpen") then
  reaper.MB("Install ReaWebAPI in UserPlugins and restart REAPER.", "ReaWebAPI", 0)
  return
end

local source = debug.getinfo(1, "S").source:sub(2)
local directory = source:match("^(.*[/\\])")
local window = reaper.ReaWebOpen(directory .. "index.html")
if window == 0 then
  reaper.MB(reaper.ReaWeb_GetLastError(), "ReaWebAPI", 0)
end
