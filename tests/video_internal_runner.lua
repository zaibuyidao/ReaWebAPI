
local r=reaper
local backend='__BACKEND__'
local root=r.GetResourcePath()
local f=assert(io.open(root..'/results.jsonl','w'))
local function log(s) f:write(s..'\n');f:flush() end
local function wait(t) local endt=r.time_precise()+t;repeat coroutine.yield() until r.time_precise()>endt end
local id=0
local function sample(label,t,force)
 if force==1 and backend=='display' then force=2 end
 id=id+1;r.SetExtState('VideoInternal','result','',false)
 r.SetExtState('VideoInternal','request',id..' '..t..' '..force,false)
 local start=r.time_precise()
 while r.GetExtState('VideoInternal','result')=='' do assert(r.time_precise()-start<4,'probe timeout');coroutine.yield() end
 log('{"phase":"'..label..'","window":'..r.GetToggleCommandState(50125)..',"monitorFX":'..r.TrackFX_GetRecCount(r.GetMasterTrack(0))..',"result":'..r.GetExtState('VideoInternal','result')..'}')
end
local co=coroutine.create(function()
 r.InsertMedia(root..'/red.png',1)
 local first=r.GetMediaItem(0,0);r.SetMediaItemInfo_Value(first,'D_LENGTH',2)
 r.SetEditCurPos(2,false,false);r.InsertMedia(root..'/blue.png',0)
 local second=r.GetMediaItem(0,1);r.SetMediaItemInfo_Value(second,'D_POSITION',2);r.SetMediaItemInfo_Value(second,'D_LENGTH',2)
 r.GetSetProjectInfo(0,'PROJECT_SRATE',48000,true)
 r.SetEditCurPos(0,false,true);r.Audio_Init();wait(1)
 for _,phase in ipairs({{'cold_closed',0,0},{'cold_forced',0,1},{'closed',0,0},{'open',1,0},{'closed_forced',0,1},{'closed_restored',0,0}}) do
  if r.GetToggleCommandState(50125)~=phase[2] then r.Main_OnCommand(50125,0) end
  r.SetEditCurPos(0,false,true);wait(0.1)
  for i=1,20 do sample(phase[1],0.5,phase[3]);wait(0.05) end
  r.SetEditCurPos(2.5,false,true)
  for i=1,20 do sample(phase[1]..'_seek',2.5,phase[3]);wait(0.05) end
 end
 local master=r.GetMasterTrack(0)
 local fx=r.TrackFX_AddByName(master,'Video processor',false,-1)
 assert(fx>=0,'master effect missing')
 assert(r.TrackFX_SetNamedConfigParm(master,fx,'VIDEO_CODE','gfx_blit(0); gfx_set(0,1,0,1); gfx_fillrect(0,0,100,100);'))
 r.SetEditCurPos(2.5,false,true)
 for i=1,20 do sample('master_overlay_closed',2.5,1);wait(.05) end
 r.TrackFX_Delete(master,fx)
 local monitoring=r.TrackFX_AddByName(master,'Video processor',true,-1)
 assert(monitoring>=0,'monitoring test effect missing')
 assert(r.TrackFX_SetNamedConfigParm(master,0x1000000+monitoring,'VIDEO_CODE','gfx_blit(0); gfx_set(0,1,0,1); gfx_fillrect(0,0,100,100);'))
 for i=1,20 do sample('monitor_effect_closed',2.5,1);wait(.05) end
 r.TrackFX_Delete(master,0x1000000+monitoring)
 r.SetEditCurPos(8,false,false);r.InsertMedia(root..'/motion.mp4',0)
 local motion=r.GetMediaItem(0,2);assert(motion,'motion missing');r.SetMediaItemInfo_Value(motion,'D_POSITION',8);r.SetMediaItemInfo_Value(motion,'D_LENGTH',10)
 r.SetEditCurPos(8,false,true);r.GetSet_LoopTimeRange(true,true,8,18,false);r.GetSetRepeat(1);r.OnPlayButton()
 for i=1,180 do sample('motion_play_closed',r.GetPlayPosition(),1);wait(.025) end
 r.OnStopButton()
 r.SetEditCurPos(12,false,true)
 for i=1,20 do sample('motion_seek_closed',12,1);wait(.05) end
 for i=1,20 do sample('restore',12,0);wait(.05) end
 for i=r.CountTracks(0)-1,0,-1 do r.DeleteTrack(r.GetTrack(0,i)) end
 for i=1,20 do sample('empty_project',0,1);wait(.05) end
 sample('cleanup',0,0)
 log('{"done":true}');f:close()
end)
local function step()local ok,err=coroutine.resume(co);if not ok then log('{"error":'..string.format('%q',tostring(err))..'}');f:close();return end;if coroutine.status(co)~='dead' then r.defer(step) end end
step()
