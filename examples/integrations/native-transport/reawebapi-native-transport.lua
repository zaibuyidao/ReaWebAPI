-- @description ReaWebAPI: Native Transport Monitor (no Lua defer)
-- @version 1.1
-- Lua opens the window and exits. The page subscribes to native transport snapshots.
if not reaper.APIExists('ReaWeb_Open') then
  reaper.MB('This demo requires ReaWebAPI v0.3.9.23 or later. Install the extension and restart REAPER.', 'Native Transport Monitor', 0)
  return
end
local source = debug.getinfo(1, 'S').source
local directory = source:sub(2):match('^(.*[/\\])')
local id = reaper.ReaWeb_Open(directory .. 'index.html', source, 'Native Transport Monitor')
if id == 0 then reaper.MB(reaper.ReaWeb_GetLastError(), 'Native Transport Monitor', 0) end
