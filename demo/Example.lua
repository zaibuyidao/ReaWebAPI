-- @description ReaWebAPI: Example
-- @version 0.1.7
-- @about Opens the bundled JavaScript REAPER API demo.

if not reaper.APIExists("ReaWeb_Open") then
  reaper.MB("Install ReaWebAPI 0.1.7 or newer in UserPlugins and restart REAPER.", "ReaWebAPI", 0)
  return
end

local source = debug.getinfo(1, "S").source:sub(2)
local directory = source:match("^(.*[/\\])")
local window = reaper.ReaWeb_Open(directory .. "index.html")
if window == 0 then
  reaper.MB(reaper.ReaWeb_GetLastError(), "ReaWebAPI", 0)
end
