local root = debug.getinfo(1, 'S').source:sub(2):match('^(.*[/\\])')
assert(reaper.GetResourcePath():gsub('[/\\]+$', '') == root:gsub('[/\\]+$', ''))
local source = root .. '示例 空格/eel-mixer/reawebapi-eel-mixer.eel'
if reaper.GetOS():match('Win') then source = source:gsub('/', '\\') end
local function log(text)
  local f=assert(io.open(root..'result.txt','a'));f:write(text..'\n');f:close()
end
log('REAPER '..reaper.GetAppVersion())
reaper.InsertTrackAtIndex(0,false)
reaper.InsertTrackAtIndex(1,false)
local track, other = reaper.GetTrack(0,0), reaper.GetTrack(0,1)
reaper.GetSetMediaTrackInfo_String(track,'P_NAME','鼓组 "A" \\ <b>🎛</b>\n\tend',true)
reaper.SetOnlyTrackSelected(track)
local action = reaper.AddRemoveReaScript(true,0,source,true)
reaper.Main_OnCommand(action,0)
local start, stage, window = reaper.time_precise(), 'start'
local function step()
  local owner = reaper.GetExtState('ReaWebAPI.EELBackends',source)
  if owner~='' then window=tonumber(owner) end
  local report = reaper.GetExtState('EEL.Mixer.Test','stage')
  if report:find('FAIL:',1,true) then log(report); return end
  if reaper.time_precise()-start>45 then log('FAIL: timeout at '..stage);return end
  if stage=='start' and report=='edited' then
    assert(math.abs(reaper.GetMediaTrackInfo_Value(track,'D_VOL')-4)<1e-8)
    assert(math.abs(reaper.GetMediaTrackInfo_Value(track,'D_PAN')+.37)<1e-8)
    assert(reaper.GetMediaTrackInfo_Value(track,'B_MUTE')==1)
    assert(reaper.GetMediaTrackInfo_Value(track,'I_SOLO')==2)
    local r,g,b=reaper.ColorFromNative(reaper.GetTrackColor(track))
    assert(r==18 and g==171 and b==52)
    assert(reaper.GetExtState('EEL.Mixer.Test','sliderChecks')=='confirmed')
    log('PASS: SetFrame state, gain 0..4, invalid range rejection, execution acks, 8-flight limit, 500-step coalescing, final drain and stale snapshots')
    log('PASS: UI volume/pan/color/mute/solo reached EEL2 and REAPER')
    reaper.SetMediaTrackInfo_Value(track,'D_VOL',3.5)
    reaper.SetMediaTrackInfo_Value(track,'D_PAN',.62)
    reaper.SetMediaTrackInfo_Value(track,'B_MUTE',0)
    reaper.SetMediaTrackInfo_Value(track,'I_SOLO',0)
    reaper.SetTrackColor(track,reaper.ColorToNative(170,34,204)|0x1000000)
    stage='external'
  elseif stage=='external' and report=='external' then
    log('PASS: external REAPER volume/pan/color/mute/solo reflected in UI')
    reaper.SetOnlyTrackSelected(other)
    reaper.SetExtState('EEL.Mixer.Test','next','stale',false)
    stage='stale'
  elseif stage=='stale' and report=='stale' then
    assert(reaper.GetMediaTrackInfo_Value(other,'D_VOL')==1)
    assert(reaper.GetMediaTrackInfo_Value(track,'D_VOL')==3.5)
    log('PASS: stale track GUID rejected')
    reaper.SetOnlyTrackSelected(track)
    reaper.SetExtState('EEL.Mixer.Test','next','reset',false)
    stage='reset'
  elseif stage=='reset' and report=='reset' then
    assert(reaper.GetTrackColor(track)==0)
    log('PASS: default color restored')
    reaper.SetExtState('EEL.Mixer.Test','next','reload',false)
    stage='reload'
  elseif stage=='reload' and report=='reloaded' then
    log('PASS: reload resubscribes to frame state with a new document session')
    reaper.SetTrackSelected(track,false)
    stage='none'
  elseif stage=='none' and report=='none' then
    log('PASS: no selection disables controls')
    reaper.ReaWeb_Close(window)
    stage='closed'
  elseif stage=='closed' and owner=='' then
    assert(not reaper.ReaWeb_IsOpen(window))
    log('PASS: close exits EEL2 and clears ownership')
    log('PASS: complete');return
  end
  reaper.defer(step)
end
step()
