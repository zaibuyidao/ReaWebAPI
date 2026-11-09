-- @noindex
-- @description ReaWebAPI: Video Preview
local source = debug.getinfo(1, 'S').source
if not reaper.APIExists('ReaWeb_Open') then
  reaper.MB('Install ReaWebAPI v0.3.9.31 or later and restart REAPER.', 'Video Preview', 0)
  return
end
local id = reaper.ReaWeb_Open(source:sub(2):match('^(.*[/\\])') .. 'index.html', source)
if id == 0 then reaper.MB(reaper.ReaWeb_GetLastError(), 'Video Preview', 0) end
