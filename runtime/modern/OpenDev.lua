if not reaper.APIExists("ReaWeb_OpenDev") then error("Install ReaWebAPI v0.1.6 or later") end
local id = reaper.ReaWeb_OpenDev("http://localhost:5173/")
if id == 0 then reaper.ShowMessageBox(reaper.ReaWeb_GetLastError(), "ReaWebAPI", 0) end
