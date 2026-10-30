local root = debug.getinfo(1, 'S').source:sub(2):match('^(.*[/\\])')
local counts, duration = {STRESS_COUNTS}, STRESS_SECONDS
local views, retired, samples, stopped = {}, {}, {}, false
local steady, defers, frame_ages = {}, {}, {}
local frames_enabled = STRESS_FRAMES
local case, heavy, phase, opened, demo, deadline, run_at, replaced = 0, STRESS_HEAVY, 'next', 0, 0, 0, 0, false
local function log(name, text)
  local f = assert(io.open(root .. name, 'a')); f:write(text .. '\n'); f:close()
end
local function check(ok, text)
  if not ok then error(text or reaper.ReaWeb_GetLastError(), 2) end
end
local function finish(text)
  if stopped then return end
  stopped = true
  for _, v in ipairs(views) do v.active = false; reaper.ReaWeb_Close(v.id) end
  if demo > 0 then reaper.ReaWeb_Close(demo) end
  reaper.OnStopButton()
  local f = assert(io.open(root .. 'result.txt', 'w')); f:write(text); f:close()
end
local function protect(fn)
  return function()
    if stopped then return end
    local ok, err = pcall(fn)
    if not ok then
      for _, v in ipairs(views) do
        log('failure-diagnostics.jsonl', reaper.ReaWeb_GetDiagnostics(v.id))
        log('failure-windows.tsv', table.concat({v.id,v.sent,v.received,v.pending,v.ready_ms or -1,v.max_ms}, '\t'))
      end
      finish('FAIL: ' .. tostring(err))
    end
  end
end
local function open_view(slot)
  local id = reaper.ReaWeb_Open(root .. 'app/index.html'); check(id > 0)
  local v = {id=id, active=true, ready=false, sent=0, received=0, flights={}, pending=0, max_ms=0, born=reaper.time_precise(), next_frame=0}
  views[slot] = v
  local function backend()
    if not v.active then return end
    check(reaper.ReaWeb_IsOpen(id), 'Unexpected window closure ' .. id)
    local now = reaper.time_precise()
    if phase == 'run' and v.last_poll then defers[#defers+1] = (now-v.last_poll)*1000 end
    v.last_poll = now
    for _ = 1, 32 do
      local text = reaper.ReaWeb_Receive(id)
      if text == '' then break end
      if text == 'ready' then
        check(not v.ready and reaper.ReaWeb_IsReady(id), 'Invalid readiness ' .. id)
        v.ready = true; v.ready_ms = (now-v.born)*1000
      else
        local payload, frame, written = text:match('^(.*)\t(%d+)\t([%d%.]+)$')
        check(payload, 'Missing frame receipt metadata')
        text = payload; v.frame = tonumber(frame)
        if phase == 'run' and tonumber(written) > 0 then frame_ages[#frame_ages+1] = (now-tonumber(written))*1000 end
        local owner, serial = text:match('^(%d+):(%d+):')
        serial = tonumber(serial)
        check(tonumber(owner) == id and serial == v.received+1, 'Window/FIFO mismatch ' .. id)
        local flight = v.flights[serial]
        check(flight and flight.text == text, 'Payload mismatch ' .. id)
        local ms = (now-flight.at)*1000
        samples[#samples+1] = ms; v.max_ms = math.max(v.max_ms, ms)
        if phase == 'run' and flight.phase == 'run' then steady[#steady+1] = ms end
        v.flights[serial] = nil; v.pending = v.pending-1; v.received = serial
      end
    end
    check(v.ready or now-v.born < 10, 'Ready timeout ' .. id)
    for _, flight in pairs(v.flights) do check(now-flight.at < 10, 'Reply timeout ' .. id) end
    if v.ready and phase ~= 'drain' and phase ~= 'closed' then
      if v.pending < 4 then
        v.sent = v.sent+1
        local text = id .. ':' .. v.sent .. ':' .. string.rep('x', v.sent%8 == 0 and 8192 or 128)
        check(reaper.ReaWeb_Send(id, text))
        v.flights[v.sent] = {at=now, text=text, phase=phase}; v.pending = v.pending+1
      end
      if now >= v.next_frame then
        local state = {}
        for i = 0, reaper.CountTracks(0)-1 do
          local track = reaper.GetTrack(0,i)
          state[#state+1] = string.format('%.3f', reaper.GetMediaTrackInfo_Value(track,'D_VOL'))
        end
        check(reaper.ReaWeb_SetFrame(id, table.concat(state, ',') .. ':' .. now))
        v.next_frame = now+0.1
      end
    end
    reaper.defer(protect(backend))
  end
  reaper.defer(protect(backend))
end
local function close_view(v)
  v.active = false
  log('diagnostics.jsonl', reaper.ReaWeb_GetDiagnostics(v.id))
  check(reaper.ReaWeb_Close(v.id)); check(not reaper.ReaWeb_IsOpen(v.id))
  retired[#retired+1] = v
end
local function load_project()
  reaper.gmem_attach('MultiWindowStress'); reaper.gmem_write(0,0)
  for i = 0, 49 do
    reaper.InsertTrackAtIndex(i, false)
    local track = reaper.GetTrack(0,i)
    reaper.GetSetMediaTrackInfo_String(track, 'P_NAME', 'Stress ' .. i, true)
    for _ = 1, i < 30 and 2 or 1 do check(reaper.TrackFX_AddByName(track, 'JS: stress-load', false, -1) >= 0, 'Cannot load stress JSFX') end
  end
  reaper.SetOnlyTrackSelected(reaper.GetTrack(0,0)); reaper.UpdateArrange()
  reaper.SetMediaTrackInfo_Value(reaper.GetMasterTrack(0), 'D_VOL', 0)
  local item = reaper.AddMediaItemToTrack(reaper.GetTrack(0,0))
  reaper.SetMediaItemLength(item, 600, false)
  local fx = 0
  for i = 0, reaper.CountTracks(0)-1 do fx = fx+reaper.TrackFX_GetCount(reaper.GetTrack(0,i)) end
  check(reaper.CountTracks(0) == 50 and fx == 80, 'Incorrect stress project')
  reaper.OnPlayButton()
end
local function step()
  local now = reaper.time_precise()
  if phase == 'next' then
    case = case+1
    if case > #counts then
      if heavy then finish('PASS: requested window matrix (see cases.tsv), continuous traffic, official Demo, close/reopen and FIFO isolation'); return end
      heavy = true; case = 1; load_project()
    end
    views, retired, samples, opened, replaced = {}, {}, {}, 0, false
    steady, defers, frame_ages = {}, {}, {}
    reaper.DeleteExtState('MultiWindowStress','demo',false)
    local script = root .. 'demo/ReaWebAPI_Demo.lua'
    dofile(script)
    demo = reaper.ReaWeb_Open(root .. 'demo/index.html', '@' .. script); check(demo > 0)
    phase = 'opening'; deadline = now+counts[case]*12+15
  elseif phase == 'opening' then
    check(now < deadline, 'Opening timeout')
    if opened == 0 or (views[opened].ready and views[opened].received > 0) then
      if opened < counts[case]-1 then opened = opened+1; open_view(opened)
      else
        if heavy then
          reaper.SetEditCurPos(0,false,true); reaper.OnPlayButton(); reaper.gmem_write(0,0)
        end
        phase = 'run'; run_at = now; deadline = now+duration+20
      end
    end
  elseif phase == 'run' then
    check(now < deadline, 'Run timeout')
    local status = reaper.GetExtState('MultiWindowStress','demo')
    check(not status:match('^FAIL:'), status)
    if now-run_at >= duration/2 and not replaced then
      for i = #views, 1, -2 do close_view(views[i]) end
      phase = 'replace'; replaced = true
    end
    if now-run_at >= duration then
      check(tonumber(status) and tonumber(status) > 5, 'Official Demo not communicating: ' .. status)
      phase = 'drain'; deadline = now+10
    end
  elseif phase == 'replace' then
    check(now < deadline, 'Replacement timeout')
    local ready = true
    for i = #views, 1, -2 do
      if not views[i].active then
        if reaper.ReaWeb_GetDiagnostics(views[i].id):find('"stage":"closed"',1,true) then open_view(i) end
        ready = false
      else ready = ready and views[i].ready and views[i].received > 0 end
    end
    if ready then phase = 'run' end
  elseif phase == 'drain' then
    check(now < deadline, 'Drain timeout')
    local drained = true
    for _, v in ipairs(views) do
      drained = drained and v.pending == 0; check(v.received > 5, 'Window did not sustain traffic ' .. v.id)
      check(not frames_enabled or (v.frame and v.frame > 5), 'Frame delivery stalled ' .. v.id)
    end
    if drained then
      if heavy then
        check(reaper.GetPlayState()&1 == 1, 'Stress project is not playing')
        check(reaper.gmem_read(0) > 0, 'Stress JSFX did not process audio blocks')
      end
      for i = 2, #views, 2 do close_view(views[i]) end
      for i = #views%2 == 1 and #views or #views-1, 1, -2 do close_view(views[i]) end
      check(reaper.ReaWeb_Close(demo)); demo = 0
      table.sort(samples)
      table.sort(steady); table.sort(defers); table.sort(frame_ages)
      local function p95(values) return values[math.max(1,math.ceil(#values*.95))] or 0 end
      log('latency.tsv',table.concat({heavy and 'heavy' or 'empty',counts[case],p95(steady),p95(defers),#steady,p95(frame_ages)},'\t'))
      local total = 0
      for _, v in ipairs(retired) do
        total = total+v.received
        log('windows.tsv', table.concat({heavy and 'heavy' or 'empty',counts[case],v.id,v.sent,v.received,v.ready_ms or -1,v.max_ms}, '\t'))
      end
      log('cases.tsv', table.concat({heavy and 'heavy' or 'empty',counts[case],reaper.CountTracks(0),total,samples[math.max(1,math.ceil(#samples*.95))] or 0,samples[#samples] or 0, reaper.GetPlayState(),heavy and reaper.gmem_read(0) or 0}, '\t'))
      phase = 'closed'; deadline = now+1
    end
  elseif phase == 'closed' and now >= deadline then
    for _, v in ipairs(retired) do
      check(not reaper.ReaWeb_IsOpen(v.id), 'Backend window remains open')
      check(not reaper.ReaWeb_Send(v.id, 'stale'), 'Closed window accepted message')
    end
    phase = 'next'
  end
  reaper.defer(protect(step))
end
check(reaper.GetResourcePath():gsub('[/\\]+$', '') == root:gsub('[/\\]+$', ''), 'Requires an isolated REAPER profile')
while reaper.CountTracks(0) > 0 do reaper.DeleteTrack(reaper.GetTrack(0,0)) end
log('cases.tsv','load\twindows\ttracks\techoes\tp95_ms\tmax_ms\tplay_state\tfx_blocks')
log('windows.tsv','load\twindows\tid\tsent\treceived\tready_ms\tmax_ms')
log('host.txt',reaper.GetAppVersion())
log('latency.tsv','load\twindows\tsteady_p95_ms\tdefer_p95_ms\tsteady_echoes\tframe_age_p95_ms')
if heavy then load_project() end
protect(step)()
