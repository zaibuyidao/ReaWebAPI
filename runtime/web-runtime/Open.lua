local source = debug.getinfo(1, 'S').source:sub(2)
local folder = source:match('^(.*[/\\])')
if not reaper.APIExists('ReaWebOpen') then
  reaper.MB('Install ReaWebAPI v0.1.6 or later.', 'Web Runtime checks', 0)
  return
end
local id = reaper.ReaWebOpen(folder .. 'index.html')
if id == 0 then reaper.MB(reaper.ReaWeb_GetLastError(), 'Web Runtime checks', 0) end
