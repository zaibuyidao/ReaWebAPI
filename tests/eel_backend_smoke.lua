local root = debug.getinfo(1, 'S').source:sub(2):match('^(.*[/\\])')
local source = root .. '示例 空格/eel-backend/reawebapi-eel-backend.eel'
if reaper.GetOS():match('Win') then source = source:gsub('/', '\\') end
local started = reaper.time_precise()
local stage = 'start'
local function log(text)
  local f = assert(io.open(root .. 'result.txt', 'a')); f:write(text .. '\n'); f:close()
end
log('REAPER ' .. reaper.GetAppVersion())
log('Resource ' .. reaper.GetResourcePath())
assert(reaper.GetResourcePath():gsub('[/\\]+$', '') == root:gsub('[/\\]+$', ''), 'Expected isolated resource directory')
assert(reaper.APIExists('ReaWeb_Open'), 'ReaWebAPI not loaded')
reaper.InsertTrackAtIndex(0, false)
local track = reaper.GetTrack(0, 0)
reaper.GetSetMediaTrackInfo_String(track, 'P_NAME', '鼓组 "A" \\ <b>🎛</b>\n\tend', true)
reaper.SetOnlyTrackSelected(track)
local action = reaper.AddRemoveReaScript(true, 0, source, true)
log('EEL action ' .. action)
reaper.Main_OnCommand(action, 0)
local window
local function step()
  if reaper.time_precise() - started > 40 then
    log('FAIL: timeout at ' .. stage .. ' ' .. reaper.GetExtState('EEL.Validation', 'progress'))
    if window then log(reaper.ReaWeb_GetDiagnostics(window)) end
    return
  end
  local owner = reaper.GetExtState('ReaWebAPI.EELBackends', source)
  if owner ~= '' then window = tonumber(owner) end
  local report = reaper.GetExtState('EEL.Validation', 'stage')
  if report:find('FAIL', 1, true) then log(report); return end
  if report == 'volume' and stage == 'start' then
    assert(reaper.GetMediaTrackInfo_Value(track, 'D_VOL') == 4)
    log('PASS: SetFrame state, gain 0..4, range rejection, 8-flight limit, 500-step coalescing, final ack and stale frame protection')
    reaper.SetMediaTrackInfo_Value(track, 'D_VOL', 3.5)
    stage = 'volume'
  elseif report == 'selected' and stage == 'volume' then
    log('PASS: external REAPER gain 3.5 reflected in UI')
    assert(window, 'EEL loop ownership not found')
    local f = assert(io.open(root .. 'diagnostics.json', 'w'))
    f:write(reaper.ReaWeb_GetDiagnostics(window)); f:close()
    log('PASS: EEL Open/IsOpen/IsReady/Receive/Send and button selected Unicode track')
    reaper.SetTrackSelected(track, false)
    reaper.SetExtState('EEL.Validation', 'next', 'none', false)
    stage = 'none'
  elseif report == 'none' and stage == 'none' then
    log('PASS: no selection')
    reaper.SetOnlyTrackSelected(track)
    reaper.SetExtState('EEL.Validation', 'next', 'reload', false)
    stage = 'reload'
  elseif report == 'reloaded' and stage == 'reload' then
    log('PASS: reload handshake and repeated button request')
    reaper.ReaWeb_Close(window)
    stage = 'closed'
  elseif stage == 'closed' and owner == '' then
    assert(not reaper.ReaWeb_IsOpen(window))
    log('PASS: window close exited EEL defer loop and ran atexit (owner removed)')
    reaper.SetExtState('EEL.Validation', 'next', 'reopen', false)
    reaper.SetExtState('EEL.Validation', 'stage', '', false)
    reaper.Main_OnCommand(action, 0)
    stage = 'reopen'
  elseif stage == 'reopen' and report == 'reopened' then
    log('PASS: same EEL action reopened and returned track name without an already-running prompt')
    reaper.ReaWeb_Close(window)
    stage = 'closed-again'
  elseif stage == 'closed-again' and owner == '' then
    assert(not reaper.ReaWeb_IsOpen(window))
    log('PASS: complete'); return
  end
  reaper.defer(step)
end
step()
