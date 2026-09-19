local directory = debug.getinfo(1, "S").source:sub(2):match("^(.*[/\\])")
if not reaper.APIExists("ReaWeb_Open") then error("Install ReaWebAPI v0.1.7 or later") end
local id = reaper.ReaWeb_Open(directory .. "dist/index.html")
if id == 0 then reaper.ShowMessageBox(reaper.ReaWeb_GetLastError(), "ReaWebAPI", 0) end
