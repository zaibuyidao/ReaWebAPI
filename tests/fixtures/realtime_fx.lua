local dir = debug.getinfo(1, 'S').source:sub(2):match('^(.*[/\\])')
local function write(name, text)
  local file = assert(io.open(dir .. name .. '.tmp', 'w'))
  file:write(text)
  file:close()
  assert(os.rename(dir .. name .. '.tmp', dir .. name))
end
reaper.InsertTrackAtIndex(0, true)
local track = reaper.GetTrack(0, 0)
local media = reaper.AddMediaItemToTrack(track)
local audio = assert(reaper.PCM_Source_CreateFromFile(dir .. 'automation.wav'))
reaper.SetMediaItemTake_Source(reaper.AddTakeToMediaItem(media), audio)
reaper.SetMediaItemInfo_Value(media, 'D_LENGTH', 8)
reaper.SetOnlyTrackSelected(track)
local index, guid, cleanup_monitoring, envelope
local function chain_targets()
  local rows = {}
  for _, chain in ipairs({'main','input','master-main','monitoring','take'}) do
    local rec = chain == 'input' or chain == 'monitoring'
    local owner = (chain == 'monitoring' or chain == 'master-main') and reaper.GetMasterTrack(0) or track
    local take = chain == 'take' and reaper.GetActiveTake(reaper.CreateNewMIDIItemInProj(track, 0, 1, false))
    local obj = take or owner
    local api = function(name) return reaper[(take and 'TakeFX_' or 'TrackFX_') .. name] end
    local function insert(name)
      local i
      if take then i = api('AddByName')(obj, name, -1)
      else i = api('AddByName')(obj, name, rec, -1) end
      assert(i >= 0, chain .. ' could not create ' .. name)
      return rec and (i | 0x1000000) or i
    end
    local parent = insert('Container')
    local effect = insert('ReaEQ')
    local count = api(rec and 'GetRecCount' or 'GetCount')(obj)
    local dest = 0x2000000 + parent + 1 + count + 1
    api(take and 'CopyToTake' or 'CopyToTrack')(obj, effect, obj, dest, true)
    local ok, child = api('GetNamedConfigParm')(obj, parent, 'container_item.0')
    assert(ok and tonumber(child), chain .. ' could not resolve nested FX')
    child = tonumber(child)
    local v = (#rows + 1) / 10
    assert(api('SetParamNormalized')(obj, child, 0, v))
    local scope = chain == 'master-main' and 'main' or chain
    local extra = ''
    if take then
      local valid, id = reaper.GetSetMediaItemTakeInfo_String(take, 'GUID', '', false)
      assert(valid)
      extra = ',"take":"' .. id .. '"'
    end
    rows[#rows+1] = string.format('{"chain":"%s","track":"%s","fx":"%s","expected":%.17g%s}',
      scope, reaper.GetTrackGUID(owner), api('GetFXGUID')(obj, child), api('GetParamNormalized')(obj, child, 0), extra)
    if chain == 'monitoring' then
      local id = api('GetFXGUID')(obj, parent)
      cleanup_monitoring = function()
        if not reaper.ValidatePtr2(0, owner, 'MediaTrack*') then return end
        for i = 0, reaper.TrackFX_GetRecCount(owner)-1 do
          if reaper.TrackFX_GetFXGUID(owner, i | 0x1000000) == id then
            reaper.TrackFX_Delete(owner, i | 0x1000000)
            break
          end
        end
      end
      reaper.atexit(cleanup_monitoring)
    end
  end
  return '[' .. table.concat(rows, ',') .. ']'
end
local function add()
  index = reaper.TrackFX_AddByName(track, 'ReaEQ', false, -1)
  assert(index >= 0, 'Could not instantiate ReaEQ')
  guid = reaper.TrackFX_GetFXGUID(track, index)
end
local function target()
  return string.format('{"track":"%s","fx":"%s"}', reaper.GetTrackGUID(track), guid)
end
local function execute(cmd)
  if cmd == 'chains' then return chain_targets() end
  if cmd == 'init' or cmd == 'replace' then
    if cmd == 'replace' then
      reaper.OnStopButton()
      reaper.TrackFX_Delete(track, index)
    end
    add()
    if cmd == 'init' then reaper.Undo_OnStateChangeEx('FX fixture ready',-1,-1) end
    return target()
  elseif cmd == 'undo-write' or cmd == 'redo-write' then
    local undo = cmd == 'undo-write'
    local label = (undo and reaper.Undo_CanUndo2 or reaper.Undo_CanRedo2)(0)
    assert(label == 'FX parameter review', tostring(label))
    if undo then reaper.Undo_DoUndo2(0) else reaper.Undo_DoRedo2(0) end
    track = reaper.GetTrack(0,0)
    assert(reaper.TrackFX_GetFXGUID(track,index) == guid)
  elseif cmd == 'finish-automation' then
    reaper.SetTrackAutomationMode(track, 0)
    local ok, chunk = reaper.GetEnvelopeStateChunk(envelope, '', false)
    assert(ok and reaper.SetEnvelopeStateChunk(envelope, chunk:gsub('\nACT 1','\nACT 0'), false))
  elseif cmd == 'reorder' then
    assert(reaper.TrackFX_AddByName(track, 'ReaEQ', false, -1000) == 0)
    index = index + 1
    assert(reaper.TrackFX_GetFXGUID(track, index) == guid)
  elseif cmd == 'bypass' then
    reaper.TrackFX_SetEnabled(track, index, false)
  elseif cmd == 'enable' then
    reaper.TrackFX_SetEnabled(track, index, true)
  elseif cmd == 'preset' then
    local _, count = reaper.TrackFX_GetPresetIndex(track, index)
    if count < 1 then return 'null' end
    assert(reaper.TrackFX_SetPresetByIndex(track, index, 0))
  elseif cmd == 'automation' then
    local points = {}
    for _, v in ipairs({0.9, 0.1}) do
      assert(reaper.TrackFX_SetParamNormalized(track, index, 0, v))
      points[v] = reaper.TrackFX_GetParam(track, index, 0)
      reaper.TrackFX_EndParamEdit(track, index, 0)
    end
    local env = assert(reaper.GetFXEnvelope(track, index, 0, true))
    envelope = env
    reaper.DeleteEnvelopePointRange(env, -1, 100)
    local mode = reaper.GetEnvelopeScalingMode(env)
    for _, point in ipairs({{0, 0.1}, {0.2, 0.9}}) do
      assert(reaper.InsertEnvelopePoint(env, point[1],
        reaper.ScaleToEnvelopeMode(mode, points[point[2]]), 0, 0, false, true))
    end
    reaper.Envelope_SortPoints(env)
    local ok, chunk = reaper.GetEnvelopeStateChunk(env, '', false)
    assert(ok)
    assert(reaper.SetEnvelopeStateChunk(env, chunk:gsub('\nACT 0','\nACT 1'), false))
    reaper.SetGlobalAutomationOverride(-1)
    reaper.SetTrackAutomationMode(track, 1)
    reaper.TrackList_AdjustWindows(false)
    reaper.UpdateArrange()
    reaper.TrackFX_Show(track, index, 3)
    reaper.SetEditCurPos(0, false, false)
    reaper.OnPlayButton()
    return 'true'
  elseif cmd == 'automation-read' then
    local pos = reaper.GetPlayPosition()
    assert(pos > 0.2, 'Audio transport did not advance')
    local _, value = reaper.Envelope_Evaluate(envelope, pos, 48000, 1)
    local ok, chunk = reaper.GetEnvelopeStateChunk(envelope, '', false)
    write('fx-envelope.txt', string.format('position=%.6f evaluated=%.9g host=%.9g mode=%d\n%s',
      pos, value, reaper.TrackFX_GetParamNormalized(track,index,0), reaper.GetTrackAutomationMode(track), ok and chunk or ''))
  elseif cmd == 'automation-seek' then
    reaper.OnStopButton()
    reaper.SetEditCurPos(1.0, false, true)
    return 'true'
  elseif cmd == 'automation-seek-read' then
    reaper.TrackFX_Show(track, index, 2)
  elseif cmd == 'switch' then
    if cleanup_monitoring then cleanup_monitoring() end
    reaper.Main_OnCommand(40859, 0)
    return 'true'
  end
  local value = ({set=0.25,reorder=0.75,bypass=0.5,enable=0.375})[cmd]
  if value then assert(reaper.TrackFX_SetParamNormalized(track, index, 0, value)) end
  return string.format('[%.17g,%.17g]', reaper.TrackFX_GetParamNormalized(track, index, 1),
    reaper.TrackFX_GetParamNormalized(track, index, 0))
end
local function poll()
  local file = io.open(dir .. 'fx-command', 'r')
  if file then
    local cmd = file:read('*a')
    file:close()
    os.remove(dir .. 'fx-command')
    local ok, reply = xpcall(function() return execute(cmd) end, debug.traceback)
    write('fx-' .. cmd .. '.txt', reply)
    if not ok or cmd == 'switch' then return end
  end
  reaper.defer(poll)
end
poll()
