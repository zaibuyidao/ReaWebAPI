if not reaper.APIExists("ReaWeb_OpenDev") then error("Install the current ReaWebAPI extension") end
local id = reaper.ReaWeb_OpenDev("http://localhost:5173/")
if id == 0 then reaper.ShowMessageBox(reaper.ReaWeb_GetLastError(), "ReaWebAPI", 0) end
