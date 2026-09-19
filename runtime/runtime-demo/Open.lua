-- @description ReaWebAPI: Runtime Studio
if not reaper.APIExists("ReaWeb_Open") then
  reaper.MB("Install the current ReaWebAPI extension and restart REAPER.", "ReaWebAPI", 0)
  return
end
local source = debug.getinfo(1, "S").source:sub(2)
local directory = source:match("^(.*[/\\])")
if reaper.ReaWeb_Open(directory .. "index.html") == 0 then
  reaper.MB(reaper.ReaWeb_GetLastError(), "ReaWebAPI", 0)
end
