local directory = debug.getinfo(1, "S").source:sub(2):match("^(.*[/\\])")
if not reaper.APIExists("ReaWebOpen") then error("Install ReaWebAPI v0.1.6 or later") end
local id = reaper.ReaWebOpen(directory .. "dist/index.html")
if id == 0 then reaper.ShowMessageBox(reaper.ReaWeb_GetLastError(), "ReaWebAPI", 0) end
