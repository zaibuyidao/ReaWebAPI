-- Driver and Lua control for the opt-in isolated-profile API comparison.
local root = debug.getinfo(1, 'S').source:sub(2):match('^(.*[/\\])')
assert(reaper.GetResourcePath():gsub('[/\\]+$', '') == root:gsub('[/\\]+$', ''))
local path, failures = root .. 'app/index.html', 0
local function append(file, text)
  local f = assert(io.open(root .. file, 'a')); f:write(text .. '\n'); f:close()
end
local function check(label, ok)
  append('lua.tsv', (ok and 'PASS' or 'FAIL') .. '\t' .. label)
  if not ok then failures = failures + 1 end
end
local function diagnostic(label, id)
  local f = assert(io.open(root .. 'lua-' .. label .. '.json', 'w'))
  f:write(reaper.ReaWeb_GetDiagnostics(id)); f:close()
end
local function last_error(label)
  local error = reaper.ReaWeb_GetLastError()
  check(label, #error > 0); append('lua.tsv', 'ERROR\t' .. label .. '\t' .. error)
end
local function close(...) for _, id in ipairs({...}) do reaper.ReaWeb_Close(id) end end
append('result.txt', 'REAPER ' .. reaper.GetAppVersion())
reaper.InsertTrackAtIndex(0,false)
local control_track = reaper.GetTrack(0,0)
local control_false = reaper.CreateNewMIDIItemInProj(control_track,2,4,false)
local control_true = reaper.CreateNewMIDIItemInProj(control_track,2,4,true)
check('Official optional const bool false',control_false and reaper.GetMediaItemInfo_Value(control_false,'D_POSITION')==2)
check('Official optional const bool true',control_true and reaper.GetMediaItemInfo_Value(control_true,'D_POSITION')==reaper.TimeMap2_QNToTime(0,2))
local a, b = reaper.ReaWeb_Open(path), reaper.ReaWeb_Open(path)
check('Open path only creates distinct', a > 0 and b > 0 and a ~= b); close(a,b)
a = reaper.ReaWeb_Open(path, 'lua-key')
b = reaper.ReaWeb_Open(path, 'lua-key', '')
local c = reaper.ReaWeb_Open(path, 'lua-key', '设置 中文')
local d = reaper.ReaWeb_Open(path, 'lua-key', '设置 中文')
check('Open key reuse and empty id', a > 0 and a == b)
check('Open named id scope and reuse', c > 0 and c == d and c ~= a)
check('Open explicit nil fourth reuse',reaper.ReaWeb_Open(path,'lua-key',nil,nil)==a)
check('Open false reuse',reaper.ReaWeb_Open(path,'lua-key','',false)==a)
check('Open nil id false reuse',reaper.ReaWeb_Open(path,'lua-key',nil,false)==a)
check('Open scoped false reuse',reaper.ReaWeb_Open(path,'lua-key','设置 中文',false)==c)
local e = reaper.ReaWeb_Open(path,'lua-key',nil,true)
local f = reaper.ReaWeb_Open(path,'lua-key','设置 中文',true)
check('Open true distinct',e>0 and e~=a)
check('Open scoped true distinct',f>0 and f~=c and f~=e)
check('Open omitted fourth preserves singleton',reaper.ReaWeb_Open(path,'lua-key')==a)
check('Open omitted fourth preserves scope',reaper.ReaWeb_Open(path,'lua-key','设置 中文')==c)
close(e,f)
close(a,c)
a,b = reaper.ReaWeb_Open(path, ''),reaper.ReaWeb_Open(path, '')
check('Open empty key creates distinct', a > 0 and b > 0 and a ~= b); close(a,b)
local options = '{"instanceKey":"lua-ex","id":"设置","multiple":false,"hidden":true}'
a,b = reaper.ReaWeb_OpenEx(path, options),reaper.ReaWeb_OpenEx(path, options)
c = reaper.ReaWeb_OpenEx(path, '{"instanceKey":"lua-ex","id":"设置","multiple":true,"hidden":true}')
d = reaper.ReaWeb_OpenEx(path, options)
check('OpenEx false reuse', a > 0 and a == b)
check('OpenEx true distinct preserves singleton', c > 0 and c ~= a and d == a); close(a,c)
a = reaper.ReaWeb_OpenEx(path, '{}'); check('OpenEx empty object', a > 0); close(a)
a = reaper.ReaWeb_OpenEx(path, '{"hidden":true,"frameless":true,"resizable":false,"transparent":true}')
check('OpenEx appearance options', a > 0); diagnostic('appearance',a); close(a)
check('OpenEx numeric bool rejected', reaper.ReaWeb_OpenEx(path, '{"multiple":1}') == 0)
last_error('OpenEx numeric bool error')
check('OpenEx unknown field rejected', reaper.ReaWeb_OpenEx(path, '{"unknown":true}') == 0)
check('OpenEx empty JSON rejected', reaper.ReaWeb_OpenEx(path, '') == 0)
check('Open missing file', reaper.ReaWeb_Open('missing-probe.html','lua-missing') == 0)
last_error('Open missing error')
local main = reaper.ReaWeb_OpenEx(path, '{"instanceKey":"lua-main","hidden":true}')
check('IsOpen', main > 0 and reaper.ReaWeb_IsOpen(main))
check('Send before ready rejected', not reaper.ReaWeb_Send(main,'too-early'))
last_error('Send before ready error')
local f = assert(io.open(root .. 'dev-url.txt')); local url = f:read('*a'); f:close()
local dev = reaper.ReaWeb_OpenDev(url); check('OpenDev', dev > 0)
local payload = '中文 🎛 "quoted" \\ slash\n\ttab'
local started, stage = reaper.time_precise(), 0
local page_ready, dev_ready, echo_seen, frame_seen, dev_echo
local function run_eel()
  append('lua.tsv', 'COMPLETE\t' .. failures .. ' failures')
  local action = reaper.AddRemoveReaScript(true, 0, root .. 'eel_api_compatibility.eel', true)
  reaper.Main_OnCommand(action, 0)
  local start = reaper.time_precise()
  local function wait()
    if reaper.GetExtState('EEL.API.Probe','done') == 'yes' then
      append('result.txt', 'COMPLETE: inspect lua.tsv, eel.tsv and diagnostic snapshots')
    elseif reaper.time_precise()-start > 45 then
      append('result.txt', 'COMPLETE: EEL INCONCLUSIVE timeout')
    else reaper.defer(wait) end
  end
  wait()
end
local function poll()
  if reaper.time_precise()-started > 30 then
    check('async timeout',false); close(main,dev); run_eel(); return
  end
  for _=1,32 do
    local message = reaper.ReaWeb_Receive(main)
    if message == 'ready' then page_ready = true end
    if message == 'echo:' .. payload then echo_seen = true end
    if message == 'frame:frame 中文 🎛' then frame_seen = true end
    message = reaper.ReaWeb_Receive(dev)
    if message == 'ready' then dev_ready = true end
    if message == 'echo:dev' then dev_echo = true end
  end
  if stage == 0 and page_ready and dev_ready then
    check('IsReady main',reaper.ReaWeb_IsReady(main))
    check('OpenDev handshake',reaper.ReaWeb_IsReady(dev)); diagnostic('hidden',main)
    check('Show',reaper.ReaWeb_Show(main)); check('Focus',reaper.ReaWeb_Focus(main)); diagnostic('shown',main)
    check('SetDocked true',reaper.ReaWeb_SetDocked(main,true)); check('IsDocked true',reaper.ReaWeb_IsDocked(main))
    check('SetDocked false returns false',not reaper.ReaWeb_SetDocked(main,false)); check('IsDocked false',not reaper.ReaWeb_IsDocked(main))
    check('DevTools accepted',reaper.ReaWeb_DevTools(main))
    check('Send Unicode',reaper.ReaWeb_Send(main,payload)); check('SetFrame',reaper.ReaWeb_SetFrame(main,'frame 中文 🎛'))
    check('Send dev',reaper.ReaWeb_Send(dev,'dev')); stage = 1
  end
  if stage == 1 and echo_seen and frame_seen and dev_echo then
    check('Receive Unicode echo',echo_seen); check('SetFrame page delivery',frame_seen); check('OpenDev round trip',dev_echo)
    check('SetFrame clear',reaper.ReaWeb_SetFrame(main,'')); check('Receive empty',reaper.ReaWeb_Receive(main)=='')
    check('Close',reaper.ReaWeb_Close(main)); check('IsOpen after close',not reaper.ReaWeb_IsOpen(main))
    check('IsReady after close',not reaper.ReaWeb_IsReady(main)); check('Show closed',not reaper.ReaWeb_Show(main))
    check('Focus closed',not reaper.ReaWeb_Focus(main)); check('DevTools closed',not reaper.ReaWeb_DevTools(main))
    check('Send closed',not reaper.ReaWeb_Send(main,'x')); last_error('Send closed error')
    check('Receive closed empty',reaper.ReaWeb_Receive(main)==''); check('SetFrame closed',not reaper.ReaWeb_SetFrame(main,'x'))
    check('Diagnostics unknown empty object',reaper.ReaWeb_GetDiagnostics(-1)=='{}')
    close(dev); run_eel(); return
  end
  reaper.defer(poll)
end
poll()
