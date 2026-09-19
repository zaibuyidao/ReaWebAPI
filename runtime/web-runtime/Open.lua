local source = debug.getinfo(1, 'S').source:sub(2)
local folder = source:match('^(.*[/\\])')
if not reaper.APIExists('ReaWeb_Open') then
  reaper.MB('Install ReaWebAPI v0.1.7 or later.', 'Web Runtime checks', 0)
  return
end
local id = reaper.ReaWeb_Open(folder .. 'index.html')
if id == 0 then reaper.MB(reaper.ReaWeb_GetLastError(), 'Web Runtime checks', 0) end
