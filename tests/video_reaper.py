"""Opt-in project-video acceptance in a new, disposable REAPER profile."""
import argparse
import configparser
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import zlib

PAGE = r'''<!doctype html><meta charset="utf-8"><title>Video acceptance</title><canvas></canvas><script>
(async () => {
  const result = {checks: [], frames: 0};
  const check = (value, name) => { if (!value) throw Error(name); result.checks.push(name); };
  const wait = async (fn, name) => { for (let i=0;i<200;i++) { if(await fn())return;await new Promise(r=>setTimeout(r,50)); } throw Error('Timeout: '+name); };
  const pixel = (frame,x=160,y=90) => frame ? Array.from(frame.bytes.slice((y*320+x)*4,(y*320+x)*4+3)) : [];
  const ready = async stream => !!stream?.latest() && (await reaper.video.getState()).status==='ready';
  let a,b;
  try {
    await reaper.lifecycle.ready;
    await reaper.window.show();await reaper.window.focus();await wait(()=>!document.hidden,'visible page');
    const saved=sessionStorage.getItem('video-acceptance-result');
    if(saved) {
      sessionStorage.removeItem('video-acceptance-result');Object.assign(result,JSON.parse(saved));
      a=await reaper.video.openStream({width:320,height:180});
      await wait(async()=>await ready(a) && pixel(a.latest()).join(',')==='210,30,60','reloaded page video');
      check(a.info.name!==result.previousStream,'reload releases previous producer');
      await a.close();a=null;result.ok=true;
      await reaper.SetExtState('VideoAcceptance','result',JSON.stringify(result),false);return;
    }
    result.capabilities = await reaper.video.getCapabilities();
    if (await reaper.GetExtState('VideoAcceptance','expectUnsupported')==='1') {
      check(!result.capabilities.available && result.capabilities.error==='VIDEO_HOST_UNSUPPORTED','backend prerequisites are unavailable');
      let rejected=false;
      try { await reaper.video.openStream(); } catch(e) { rejected=e.code==='VIDEO_HOST_UNSUPPORTED'; }
      check(rejected,'unavailable backend rejects video');
      check((await reaper.window.getSize()).width>0,'window API remains available');
      check(typeof (await reaper.stream.getDiagnostics()).allocatedBytes==='number','stream API remains available');
      check(await reaper.TrackFX_GetRecCount(await reaper.GetMasterTrack(0))===0,'unsupported host adds no monitoring FX');
      check(await reaper.GetToggleCommandState(50125)===0,'unsupported host leaves native video window closed');
      result.ok=true;
      await reaper.SetExtState('VideoAcceptance','result',JSON.stringify(result),false);return;
    }
    check(result.capabilities.available && result.capabilities.backend==='reaper-internal', 'internal backend available');
    check(!result.capabilities.requiresCompanion && !result.capabilities.requiresVideoWindow,'no companion or video-window dependency');
    check(await reaper.GetToggleCommandState(50125)===0,'native video window never opened');
    check(await reaper.TrackFX_GetRecCount(await reaper.GetMasterTrack(0))===0,'no monitoring capture FX');
    check(result.capabilities.defaultUpdateRate==='auto' && result.capabilities.autoUpdateRate,'auto is the default update mode');
    a = await reaper.video.openStream({width:320,height:180});
    a.on('data', frame => { result.frames++; });
    await wait(async()=>await ready(a) && pixel(a.latest()).join(',')==='210,30,60', 'initial stopped frame');
    result.initial = {pixel:pixel(a.latest()),timestamp:a.latest().timestamp};
    check(pixel(a.latest()).join(',')==='210,30,60','red source pixels');
    const [projectFPS] = await reaper.TimeMap_curFrameRate(0);
    const expectedFPS=Number(await reaper.GetExtState('VideoAcceptance','projectFPS'));
    if(expectedFPS>0)check(Math.abs(projectFPS-expectedFPS)<0.001,'requested project frame rate loaded');
    result.projectFPS=projectFPS;
    const autoState=await reaper.video.getState();
    check(autoState.updateRate==='auto' && Math.abs(autoState.targetUpdateRate-Math.min(60,Math.max(1,projectFPS)))<0.0001,'default target follows native project frame rate');
    check(a.info.updateRate===autoState.targetUpdateRate,'stream descriptor preserves fractional target');
    let fixedConflict=false;
    try{await reaper.video.openStream({width:320,height:180,updateRate:30});}catch(e){fixedConflict=e.code==='VIDEO_PROFILE_CONFLICT';}
    check(fixedConflict,'fixed and auto modes cannot share one profile');
    b = await reaper.video.openStream({width:320,height:180,updateRate:'auto'});
    check(a.info.name===b.info.name,'shared source');
    await wait(()=>ready(b), 'late subscriber latest frame');
    let conflict=false;
    try { await reaper.video.openStream({width:640,height:360}); } catch(e) { conflict=e.code==='VIDEO_PROFILE_CONFLICT'; }
    check(conflict,'bounded resolution policy');
    await reaper.SetEditCurPos(2.5,false,true);
    await wait(()=>a.latest()?.timestamp>=2.49 && pixel(a.latest()).join(',')==='30,80,220','stopped seek and decoder refresh');
    result.seek = {pixel:pixel(a.latest()),timestamp:a.latest().timestamp};
    check(pixel(a.latest()).join(',')==='30,80,220','blue source pixels');
    await reaper.SetEditCurPos(6,false,true);
    await wait(async()=>{const state=await reaper.video.getState();return state.sourceWidth===0 && state.status!=='ready' && pixel(a.latest()).join(',')==='0,0,0';},'gap clears stale pixels');
    check(true,'gap publishes black invalidation and unavailable source');
    await reaper.SetEditCurPos(2.5,false,true);
    await wait(async()=>pixel(a.latest()).join(',')==='30,80,220' && (await reaper.video.getState()).status==='ready','source resumes after gap');
    check(true,'valid video resumes after invalidation');
    await reaper.InsertTrackAtIndex(0,true);
    const overlayTrack=await reaper.GetTrack(0,0);
    await reaper.SetOnlyTrackSelected(overlayTrack);
    await reaper.SetEditCurPos(0,false,true);
    await reaper.InsertMedia((await reaper.GetResourcePath())+'/red.png',0);
    const overlayItem=await reaper.GetMediaItem(0,0);
    await reaper.SetMediaItemInfo_Value(overlayItem,'D_LENGTH',4);
    await reaper.SetEditCurPos(2.5,false,true);
    await wait(()=>pixel(a.latest()).join(',')==='210,30,60','overlapping upper track');
    await reaper.SetMediaItemInfo_Value(overlayItem,'D_POSITION',4);
    await reaper.SetEditCurPos(2.55,false,true);
    await wait(()=>pixel(a.latest()).join(',')==='30,80,220','moved upper item reveals lower track');
    await reaper.DeleteTrack(overlayTrack);
    check(true,'multiple tracks, overlapping items and item move/delete');
    const master=await reaper.GetMasterTrack(0);
    const effect=await reaper.TrackFX_AddByName(master,'Video processor',false,-1);
    check(effect>=0,'master video processor');
    check(await reaper.TrackFX_SetNamedConfigParm(master,effect,'VIDEO_CODE',
      'gfx_blit(0); gfx_set(0,1,0,1); gfx_fillrect(0,0,100,100);'), 'set video overlay');
    await reaper.SetEditCurPos(2.6,false,true);
    await wait(()=>pixel(a.latest(),10,10).join(',')==='0,255,0','final master video overlay');
    check(pixel(a.latest()).join(',')==='30,80,220','overlay preserves underlying item');
    await reaper.TrackFX_SetNamedConfigParm(master,effect,'VIDEO_CODE',
      'gfx_blit(0); gfx_set(1,1,1,1); gfx_setfont(48,"Arial"); gfx_str_draw("VIDEO",0,0);');
    await reaper.SetEditCurPos(2.7,false,true);
    const hasWhite = () => { const p=a.latest().bytes;for(let i=0;i<320*40*4;i+=4)if(p[i]>245&&p[i+1]>245&&p[i+2]>245)return true;return false; };
    await wait(hasWhite,'rendered subtitle');check(true,'subtitle pixels');
    await reaper.TrackFX_SetNamedConfigParm(master,effect,'VIDEO_CODE',
      'gfx_blit(0); gfx_set(0,0,0,0.5); gfx_fillrect(0,0,project_w,project_h);');
    await reaper.SetEditCurPos(2.8,false,true);
    await wait(()=>{const p=pixel(a.latest());return p[0]>=13&&p[0]<=17&&p[2]>=107&&p[2]<=113;},'video fade');
    check(true,'fade pixels');
    await reaper.TrackFX_Delete(master,effect);
    check(await reaper.TrackFX_GetRecCount(master)===0,'master processing adds no capture FX');
    const monitor=await reaper.TrackFX_AddByName(master,'Video processor',true,-1);
    check(monitor>=0,'existing monitoring video effect');
    const monitorIndex=0x1000000+monitor;
    check(await reaper.TrackFX_SetNamedConfigParm(master,monitorIndex,'VIDEO_CODE',
      'gfx_blit(0); gfx_set(0,1,0,1); gfx_fillrect(0,0,100,100);'),'set monitoring video overlay');
    await reaper.SetEditCurPos(2.65,false,true);
    await wait(()=>pixel(a.latest(),10,10).join(',')==='0,255,0','final monitoring video overlay');
    check(await reaper.TrackFX_GetRecCount(master)===1,'existing monitoring effect remains the only monitoring FX');
    await reaper.TrackFX_Delete(master,monitorIndex);
    await reaper.SetEditCurPos(0,false,true);
    await reaper.OnPlayButton();
    await reaper.GetSet_LoopTimeRange(true,true,0,3,false);
    await reaper.GetSetRepeat(1);
    await reaper.CSurf_OnPlayRateChange(1.5);
    let previous=-1,wraps=0;
    const loopListener=b.on('data',f=>{if(previous-f.timestamp>1)++wraps;previous=f.timestamp;});
    await wait(()=>wraps>=2,'loop and 1.5x rate');loopListener();
    check(true,'loop boundary and changed play rate');
    await reaper.GetSetRepeat(0);await reaper.CSurf_OnPlayRateChange(1);
    const before=result.frames;
    await wait(()=>result.frames>before+15,'playback frames');
    await a.close();a=null;
    await wait(()=>b.latest()?.timestamp>1,'second subscriber survives close');
    check((await reaper.video.getState()).subscribers===1,'independent detach');
    await reaper.OnPauseButton();
    check((await reaper.GetPlayState() & 2)!==0,'pause');
    await reaper.OnPlayButton();
    await new Promise(r=>setTimeout(r,150));
    await reaper.OnStopButton();
    await reaper.SetEditCurPos(2.5,false,true);
    await wait(async()=>await ready(b) && b.latest()?.timestamp>=2.49 && pixel(b.latest()).join(',')==='30,80,220','stop final frame');
    result.diagnostics=await reaper.video.getDiagnostics();
    check(result.diagnostics.transportLatencyMs>0,'measured frame transport acknowledgement');
    check(await reaper.GetToggleCommandState(50125)===0,'native window stayed closed through playback');
    await reaper.Main_OnCommand(50125,0);
    check(await reaper.GetToggleCommandState(50125)===1,'user opens native video window');
    await reaper.SetEditCurPos(0.5,false,true);
    await wait(()=>pixel(b.latest()).join(',')==='210,30,60','preview alongside native video window');
    await b.close();b=null;
    await wait(async()=>!(await reaper.video.getState()).stream,'last consumer releases source');
    check(await reaper.GetToggleCommandState(50125)===1,'closing stream preserves user video window');
    a=await reaper.video.openStream({width:320,height:180});
    await wait(()=>ready(a),'reopen with native video window');
    await reaper.Main_OnCommand(50125,0);
    await reaper.SetEditCurPos(2.5,false,true);
    await wait(()=>pixel(a.latest()).join(',')==='30,80,220','preview survives user closing native video window');
    check(await reaper.GetToggleCommandState(50125)===0,'closed native video window stays closed');
    await a.close();a=null;
    await wait(async()=>!(await reaper.video.getState()).stream,'closed native window cleanup');
    check(await reaper.TrackFX_GetRecCount(master)===0,'cleanup preserves empty monitoring chain');
    for(let i=0;i<5;i++) {
      a=await reaper.video.openStream({width:320,height:180,updateRate:10});
      await wait(()=>ready(a),'reopened stopped frame');
      await a.close();a=null;
      await wait(async()=>!(await reaper.video.getState()).stream,'reopen cleanup');
    }
    check(true,'five open/close cycles');
    const duration=Number(await reaper.GetExtState('VideoAcceptance','duration'));
    if(duration>0) {
      a=await reaper.video.openStream();
      const canvas=document.querySelector('canvas');canvas.width=1280;canvas.height=720;
      const ctx=canvas.getContext('2d');let frames=0,painted=0,pending,raf=0;const hashes=new Set();
      a.on('data',f=>{++frames;let hash=0;for(let i=0;i<f.bytes.length;i+=4093)hash=(hash*31+f.bytes[i])|0;hashes.add(hash);pending=f;if(!raf)raf=requestAnimationFrame(()=>{
        raf=0;ctx.putImageData(new ImageData(new Uint8ClampedArray(pending.bytes.buffer,pending.bytes.byteOffset,pending.bytes.byteLength),1280,720),0,0);++painted;
      });});
      await reaper.SetEditCurPos(8,false,true);
      await reaper.GetSet_LoopTimeRange(true,true,8,18,false);await reaper.GetSetRepeat(1);
      const cpuBefore=(await reaper.debug.getDiagnostics()).system.processCpuSeconds;
      const begin=performance.now();await reaper.OnPlayButton();
      await new Promise(r=>setTimeout(r,duration*1000));
      const elapsed=(performance.now()-begin)/1000;
      const cpuAfter=(await reaper.debug.getDiagnostics()).system.processCpuSeconds;
      result.performance={seconds:elapsed,frames,painted,distinctFrames:hashes.size,receivedFPS:frames/elapsed,hostCpuPercent:100*(cpuAfter-cpuBefore)/elapsed,streamAllocatedBytes:(await reaper.stream.getDiagnostics()).allocatedBytes,diagnostics:await reaper.video.getDiagnostics()};
      check(frames>duration*5&&painted>0&&hashes.size>30,'sustained H.264 decode and Canvas rendering');
      await reaper.OnStopButton();await reaper.GetSetRepeat(0);await a.close();a=null;
      if(raf)cancelAnimationFrame(raf);
      await wait(async()=>!(await reaper.video.getState()).stream,'sustained playback cleanup');
    }
    await reaper.SetEditCurPos(0.5,false,true);
    a=await reaper.video.openStream({width:320,height:180});
    await wait(()=>ready(a),'window lifecycle initial frame');
    await reaper.window.setDocked(true);check(await reaper.window.isDocked(),'docked preview');
    await reaper.window.setDocked(false);await reaper.window.setSize(720,480);
    await new Promise(r=>setTimeout(r,300));
    await wait(()=>!document.hidden,'visible after docking');
    if(a.closed || !(await reaper.video.getState()).stream) {
      await a.close();a=await reaper.video.openStream({width:320,height:180});
    }
    await reaper.SetEditCurPos(0.5,false,true);
    await wait(async()=>await ready(a) && a.latest()?.timestamp<1 && pixel(a.latest()).join(',')==='210,30,60','floating resized preview');
    await reaper.window.hide();
    await new Promise(r=>setTimeout(r,250));
    await reaper.window.show();
    await wait(async()=>!(await reaper.video.getState()).stream,'hidden window releases stream');
    a=await reaper.video.openStream({width:320,height:180});
    await wait(()=>ready(a),'show and reopen');
    for(let i=await reaper.CountTracks(0)-1;i>=0;i--)await reaper.DeleteTrack(await reaper.GetTrack(0,i));
    await wait(async()=>{const state=await reaper.video.getState();return state.sourceWidth===0 && state.status!=='ready' && pixel(a.latest()).join(',')==='0,0,0';},'deleting all tracks clears stale frame');
    check(true,'empty current project clears cached video');
    await reaper.SetEditCurPos(0,false,true);
    await reaper.InsertMedia((await reaper.GetResourcePath())+'/red.png',1);
    await reaper.SetMediaItemInfo_Value(await reaper.GetMediaItem(0,0),'D_LENGTH',2);
    await reaper.SetEditCurPos(0.5,false,true);
    await wait(()=>pixel(a.latest()).join(',')==='210,30,60','video source restored after deleting all tracks');
    await reaper.Main_OnCommand(40859,0);
    await wait(async()=>(await reaper.video.getState()).error==='VIDEO_PROJECT_CHANGED','project switch');
    check(true,'project switch closes previous source');
    a=await reaper.video.openStream({width:320,height:180});
    await new Promise(r=>setTimeout(r,300));
    await wait(async()=>['waiting','unavailable'].includes((await reaper.video.getState()).status),'empty project availability');
    check(true,'empty project is not reported as a valid video source');
    await wait(()=>a.latest() && pixel(a.latest()).join(',')==='0,0,0','new empty project clears page pixels');
    check(true,'project switch cannot expose old source pixels');
    await a.close();a=null;
    await reaper.CountTracks(0).catch(e=>{if(e.code!=='PROJECT_CHANGED')throw e;});
    await reaper.Main_OnCommand(40860,0);
    await reaper.CountTracks(0).catch(e=>{if(e.code!=='PROJECT_CHANGED')throw e;});
    a=await reaper.video.openStream({width:320,height:180});await wait(()=>ready(a),'before reload');
    result.previousStream=a.info.name;a=null;
    sessionStorage.setItem('video-acceptance-result',JSON.stringify(result));
    reaper.window.reload().catch(()=>{});return;
  } catch(e) { result.ok=false;result.error=String(e);result.stack=e.stack;result.failureState=await reaper.video.getDiagnostics().catch(()=>null);result.playState=await reaper.GetPlayState().catch(()=>null);result.playPosition=await reaper.GetPlayPosition().catch(()=>null); }
  finally { await a?.close().catch(()=>{});await b?.close().catch(()=>{}); }
  await reaper.SetExtState('VideoAcceptance','result',JSON.stringify(result),false);
})();
</script>'''

RUNNER = r'''
local r=reaper
local root=r.GetResourcePath()
local function write(name,text) local f=assert(io.open(root..'/'..name,'w'));f:write(text);f:close() end
local function main()
 assert(r.APIExists('ReaWeb_Open'),'extension not loaded')
 assert(r.GetToggleCommandState(50125)==0,'native video window must start closed')
 assert(r.TrackFX_GetRecCount(r.GetMasterTrack(0))==0,'monitoring chain must start empty')
 r.SetExtState('VideoAcceptance','expectUnsupported','__EXPECT_UNSUPPORTED__',false)
 r.SetExtState('VideoAcceptance','projectFPS','__PROJECT_FPS__',false)
 r.InsertMedia(root..'/red.png',1)
 local first=r.GetMediaItem(0,0);assert(first,'PNG import failed')
 r.SetMediaItemInfo_Value(first,'D_POSITION',0);r.SetMediaItemInfo_Value(first,'D_LENGTH',2)
 r.SetEditCurPos(2,false,false);r.InsertMedia(root..'/blue.png',0)
 local second=r.GetMediaItem(0,1);assert(second,'second PNG import failed')
 r.SetMediaItemInfo_Value(second,'D_POSITION',2);r.SetMediaItemInfo_Value(second,'D_LENGTH',2)
 local media=io.open(root..'/motion.mp4','rb')
 if media then
  media:close();r.SetEditCurPos(8,false,false);r.InsertMedia(root..'/motion.mp4',0)
  local motion=r.GetMediaItem(0,2);assert(motion,'H.264 import failed')
  r.SetMediaItemInfo_Value(motion,'D_POSITION',8);r.SetMediaItemInfo_Value(motion,'D_LENGTH',10)
  r.SetExtState('VideoAcceptance','duration','__DURATION__',false)
 end
 r.SetEditCurPos(0,false,true);r.Audio_Init()
 assert(r.GetToggleCommandState(50125)==0,'test setup opened video window')
 local id=r.ReaWeb_Open(root..'/app/index.html');assert(id>0,r.ReaWeb_GetLastError())
 local begin=r.time_precise()
 local function poll()
  local result=r.GetExtState('VideoAcceptance','result')
  if result~='' then
   write('result.json',result);write('host.json','{"reaper":"'..r.GetAppVersion()..'","monitorFX":'..r.TrackFX_GetRecCount(r.GetMasterTrack(0))..'}')
   r.ReaWeb_Close(id);return
  end
  if r.time_precise()-begin>__TIMEOUT__ then write('error.txt','Page timeout\n'..r.ReaWeb_GetDiagnostics(id));return end
  r.defer(poll)
 end
 r.defer(poll)
end
local ok,err=xpcall(main,debug.traceback);if not ok then write('error.txt',err) end
'''

def png(path, rgb):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 640, 360, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress((b'\0' + bytes(rgb) * 640) * 360)) + chunk(b'IEND', b''))

def main():
    parser=argparse.ArgumentParser()
    for name in ('profile','reaper','extension'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--media',type=Path)
    parser.add_argument('--duration',type=int,default=60)
    parser.add_argument('--project-fps',type=float)
    parser.add_argument('--expect-unsupported',action='store_true', help='Expect a module fingerprint mismatch or missing backend prerequisites, not an unlisted REAPER version')
    parser.add_argument('--resource-source',type=Path)
    args=parser.parse_args()
    root=args.profile.resolve();root.mkdir(parents=True,exist_ok=False)
    (root/'UserPlugins').mkdir();(root/'EmptyPlugins').mkdir();(root/'app').mkdir()
    shutil.copy2(args.extension,root/'UserPlugins'/args.extension.name)
    source=args.reaper.resolve().parent
    resource_source=args.resource_source.resolve() if args.resource_source else source
    executable=args.reaper.resolve()
    config='[reaper]\nloadlastproj=0\nshowlastproj=0\nsplash=0\nverchk=0\nerrnowarn=5\nhasrecentlyopened=1\nnewprojdo=0\nvstfullstate=58181\nvstpath64=\nvstpath=\n'
    if args.project_fps is not None:config+=f'projfrbase={round(args.project_fps)}\nprojfrdrop={int(args.project_fps != round(args.project_fps))}\n'
    if sys.platform=='win32':
        executable=root/'reaper.exe';shutil.copy2(args.reaper,executable)
        (root/'Plugins').mkdir()
        for name in ('reaper_video.dll','reaper_wave.dll','FFmpeg.dll'):
            shutil.copy2(source/'Plugins'/name,root/'Plugins'/name)
        config+='clap_path_win64='+str(root/'EmptyPlugins')+'\n[audioconfig]\nmode=4\nwaveout_devicein=-1\nwaveout_deviceout=0\nwaveout_srate=48000\nwaveout_bps=16\nwaveout_bs=256\nwaveout_numblocks=4\nwaveout_nch_out=2\n'
    elif sys.platform=='darwin':
        resource_source=args.resource_source or Path.home()/'Library/Application Support/REAPER'
        previous=configparser.RawConfigParser(strict=False);previous.read(resource_source/'reaper.ini')
        if previous.has_section('reaper'):
            config+=''.join(k+'='+v+'\n' for k,v in previous['reaper'].items() if k.startswith('coreaudio'))
        for path in (root/'UserPlugins').iterdir():subprocess.run(['codesign','--force','--sign','-',str(path)],check=True)
    else:
        config+='linux_audio_mode=3\nlinux_audio_srate=48000\nlinux_audio_bsize=512\nlinux_audio_nch_out=2\n'
        helper=args.extension.parent/args.extension.name.replace('reaper_reawebapi-','reawebapi-webview-').removesuffix('.so')
        shutil.copy2(helper,root/'UserPlugins'/helper.name)
    for name in ('reaper-license.rk','reaper-install-rev.txt'):
        if (resource_source/name).exists():shutil.copy2(resource_source/name,root/name)
    png(root/'red.png',(210,30,60));png(root/'blue.png',(30,80,220))
    if args.media:shutil.copy2(args.media,root/'motion.mp4')
    config+='[reaper_video]\nvisible=0\n'
    (root/'reaper.ini').write_text(config)
    (root/'ReaWebAPI.ini').write_text('[Diagnostics]\nRuntimeDebug=1\nAppDebug=1\n')
    (root/'app/app.json').write_text('{"id":"video-acceptance","entry":"index.html"}')
    (root/'app/index.html').write_text(PAGE,encoding='utf-8')
    timeout=180+(args.duration if args.media else 0)
    (root/'runner.lua').write_text(RUNNER.replace('__DURATION__',str(args.duration)).replace('__TIMEOUT__',str(timeout-10)).replace('__EXPECT_UNSUPPORTED__','1' if args.expect_unsupported else '0').replace('__PROJECT_FPS__',str(args.project_fps or 0)))
    env=os.environ.copy();env['CLAP_PATH']=str(root/'EmptyPlugins')
    with (root/'process.log').open('w') as log:
        process=subprocess.Popen([str(executable),'-newinst','-cfgfile',str(root/'reaper.ini'),'-new','-ignoreerrors','-nosplash',str(root/'runner.lua')],cwd=root,env=env,stdout=log,stderr=log)
        try:
            for _ in range(timeout*2):
                if (root/'result.json').exists() or (root/'error.txt').exists():break
                if process.poll() is not None:break
                time.sleep(.5)
        finally:
            if process.poll() is None:
                process.terminate()
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.wait()
    if (root/'error.txt').exists():raise RuntimeError((root/'error.txt').read_text())
    result=json.loads((root/'result.json').read_text())
    print(json.dumps(result,indent=2))
    if not result['ok']:raise SystemExit(1)

if __name__=='__main__':main()
