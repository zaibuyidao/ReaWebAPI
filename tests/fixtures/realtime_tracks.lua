-- Disposable host fixture. The timer drives REAPER edits only.
local dir = debug.getinfo(1, 'S').source:sub(2):match('^(.*[/\\])')
reaper.InsertTrackAtIndex(0, true)
reaper.InsertTrackAtIndex(1, true)
local target = reaper.GetTrack(0, 0)
local guid = reaper.GetTrackGUID(target)
reaper.SetOnlyTrackSelected(target)
local function command(cmd)
  if cmd == 'reorder' then
    reaper.ReorderSelectedTracks(2, 0)
    target = reaper.GetTrack(0, 1)
    assert(target and reaper.GetTrackGUID(target) == guid, 'Track GUID mismatch after reorder')
    reaper.SetMediaTrackInfo_Value(target, 'D_VOL', 0.375)
    reaper.SetMediaTrackInfo_Value(target, 'D_PAN', -0.75)
    reaper.SetMediaTrackInfo_Value(target, 'B_MUTE', 1)
    reaper.SetMediaTrackInfo_Value(target, 'I_SOLO', 2)
    local out = assert(io.open(dir .. 'fixture-readback', 'w'))
    out:write(string.format('[%.17g,%.17g,%d,%d]', reaper.GetMediaTrackInfo_Value(target,'D_VOL'),
      reaper.GetMediaTrackInfo_Value(target,'D_PAN'), reaper.GetMediaTrackInfo_Value(target,'B_MUTE'),
      reaper.GetMediaTrackInfo_Value(target,'I_SOLO')))
    out:close()
  elseif cmd == 'delete' then
    reaper.DeleteTrack(target)
    reaper.SetOnlyTrackSelected(reaper.GetTrack(0,0))
  elseif cmd == 'switch' then
    reaper.Main_OnCommand(40859, 0)
  end
end
local function fixture()
  local f = io.open(dir .. 'fixture-command', 'r')
  if f then
    local cmd = f:read('*a')
    f:close()
    os.remove(dir .. 'fixture-command')
    local ok, err = xpcall(function() command(cmd) end, debug.traceback)
    local path = dir .. 'fixture-' .. cmd .. '.txt'
    local reply = assert(io.open(path .. '.tmp', 'w'))
    reply:write(ok and 'ok' or tostring(err))
    reply:close()
    assert(os.rename(path .. '.tmp', path))
    if not ok or cmd == 'switch' then return end
  end
  reaper.defer(fixture)
end
fixture()
