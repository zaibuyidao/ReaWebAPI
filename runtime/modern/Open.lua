local directory = debug.getinfo(1, "S").source:sub(2):match("^(.*[/\\])")
if not reaper.APIExists("ReaWeb_Open") then error("Install the current ReaWebAPI extension") end
local id = reaper.ReaWeb_Open(directory .. "dist/index.html")
if id == 0 then reaper.ShowMessageBox(reaper.ReaWeb_GetLastError(), "ReaWebAPI", 0) end
