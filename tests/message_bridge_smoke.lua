local root = debug.getinfo(1, "S").source:sub(2):match("^(.*[/\\])")
local ids, finished = {}, false
local function finish(result)
  if finished then return end
  finished = true
  for _, id in ipairs(ids) do reaper.ReaWeb_Close(id) end
  local file = assert(io.open(root .. 'result.txt', 'w'))
  file:write(result)
  file:close()
end
local function check(value, message)
  if not value then error(message or reaper.ReaWeb_GetLastError(), 2) end
end
local function start()
  check(reaper.APIExists('ReaWeb_Send'), 'Message API unavailable')
  check(reaper.GetResourcePath():gsub('[/\\]+$', '') == root:gsub('[/\\]+$', ''), 'Expected isolated resource directory')
  local html = assert(io.open(root .. 'bridge.html', 'w'))
  html:write([=[<!doctype html><meta charset="utf-8"><title>Message bridge test</title><script>
  (async () => {
    await reaper.events.on('message', async text => {
      if (text === 'reload') { await reaper.window.reload(); return; }
      await reaper.host.send('echo:' + text);
    });
    await reaper.host.send({type:'ready'});
  })();
  </script>]=])
  html:close()
  for i = 1, 2 do
    ids[i] = reaper.ReaWeb_Open(root .. 'bridge.html')
    check(ids[i] > 0)
    check(reaper.ReaWeb_Receive(ids[i]) == '', 'Empty receive must return a string')
  end
  local ready, index = {}, {1, 1}
  local payloads = {'A', '鼓组 🎛\n"\\', '{"type":"state"}', '', string.rep('x', 8192), 'C'}
  local stage, track, reload_ready = 'ready', nil, false
  local started = reaper.time_precise()
  local function step()
    check(reaper.time_precise() - started < 45, 'Smoke test timed out in ' .. stage)
    if stage == 'example' then
      if reaper.GetMediaTrackInfo_Value(track, 'D_VOL') == 0.5 then
        finish('PASS: real Lua ABI, Unicode, JSON serialization, FIFO, independent windows, reload, closed-window failure and Lua backend UI volume update')
      end
      return
    end
    for i, id in ipairs(ids) do
      for _ = 1, 32 do
        local message = reaper.ReaWeb_Receive(id)
        if message == '' then break end
        if stage == 'ready' then
          check(message == '{"type":"ready"}', 'Expected JSON ready message')
          ready[i] = true
        elseif stage == 'echo' then
          check(message == 'echo:' .. i .. ':' .. payloads[index[i]], 'FIFO or window isolation mismatch')
          index[i] = index[i] + 1
        elseif stage == 'reload' then
          check(i == 1 and message == '{"type":"ready"}', 'Unexpected message after reload')
          reload_ready = true
        elseif stage == 'fresh' then
          check(i == 1 and message == 'echo:fresh', 'Stale message after reload')
          stage = 'done'
        end
      end
    end
    if stage == 'ready' and ready[1] and ready[2] then
      for i, id in ipairs(ids) do
        for _, text in ipairs(payloads) do check(reaper.ReaWeb_Send(id, i .. ':' .. text)) end
      end
      stage = 'echo'
    elseif stage == 'echo' and index[1] > #payloads and index[2] > #payloads then
      check(reaper.ReaWeb_Send(ids[1], 'reload'))
      stage = 'reload'
    elseif stage == 'reload' and reload_ready then
      check(reaper.ReaWeb_Send(ids[1], 'fresh'))
      stage = 'fresh'
    elseif stage == 'done' then
      for _, id in ipairs(ids) do
        check(reaper.ReaWeb_Close(id))
        check(not reaper.ReaWeb_Send(id, 'closed'), 'Closed send must fail')
      end
      ids = {}
      reaper.InsertTrackAtIndex(0, true)
      track = reaper.GetTrack(0, 0)
      reaper.SetOnlyTrackSelected(track)
      reaper.GetSetMediaTrackInfo_String(track, 'P_NAME', '鼓组 "A"\n\t🎛', true)
      reaper.SetMediaTrackInfo_Value(track, 'D_VOL', 1)
      dofile(root .. 'example/Open.lua')
      stage = 'example'
    end
  end
  local function tick()
    local ok, err = pcall(step)
    if not ok then finish('FAIL: ' .. tostring(err)) end
    if not finished then reaper.defer(tick) end
  end
  tick()
end
local ok, err = pcall(start)
if not ok then finish('FAIL: ' .. tostring(err)) end
