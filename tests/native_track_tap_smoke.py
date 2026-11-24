"""Exercise live track taps in an isolated REAPER resource profile."""
import argparse
import configparser
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import wave


def prepare(root, extension, check_continuity=False):
    root.mkdir(mode=0o700, parents=True, exist_ok=False)
    plugins = root / 'UserPlugins'
    plugins.mkdir()
    binary = plugins / extension.name
    shutil.copy2(extension, binary)
    if sys.platform == 'darwin':
        subprocess.run(['codesign', '--force', '--sign', '-', str(binary)], check=True)
    elif sys.platform.startswith('linux'):
        for helper in extension.parent.glob('reawebapi-webview-*'):
            shutil.copy2(helper, plugins)
    (root / 'EmptyVST').mkdir()
    settings = ('[reaper]\nloadlastproj=0\nshowlastproj=0\nsplash=0\nverchk=0\nerrnowarn=5\n'
                'hasrecentlyopened=1\nnewprojdo=0\naudioclosestop=0\naudiocloseinactive=0\n'
                f'vst_scan=0\nvstpath={root / "EmptyVST"}\nvstpath64={root / "EmptyVST"}\n')
    if sys.platform == 'darwin':
        profile = Path.home() / 'Library/Application Support/REAPER'
        config = configparser.RawConfigParser(strict=False)
        config.read(profile / 'reaper.ini', encoding='utf-8-sig')
        for section in config.sections():
            if section.lower() == 'reaper':
                settings += ''.join(f'{k}={v}\n' for k, v in config[section].items() if k.startswith('coreaudio'))
        for name in ('reaper-install-rev.txt', 'reaper-license.rk'):
            if (profile / name).is_file():
                shutil.copy2(profile / name, root / name)
    elif sys.platform.startswith('linux'):
        settings += 'linux_audio_mode=3\nlinux_audio_srate=48000\nlinux_audio_bsize=256\nlinux_audio_nch_out=2\n'
    else:
        settings += '[audioconfig]\nmode=4\n'
        if check_continuity:
            settings += 'dummy_srate=48000\ndummy_blocksize=1024\n'
    (root / 'reaper.ini').write_text(settings, encoding='utf-8')
    (root / 'Effects').mkdir()
    (root / 'Effects' / 'rwa_test_gain').write_text(
        'desc:Track capture test gain\n@sample\nspl0*=2; spl1*=2;\n', encoding='utf-8')
    for name, amplitude in [('target', 4096), ('other', 2048)]:
        with wave.open(str(root / (name + '.wav')), 'wb') as out:
            out.setparams((2, 2, 48000, 0, 'NONE', ''))
            cycle = [int(amplitude * math.sin(2 * math.pi * i / 125)) for i in range(125)]
            for scale in (1, 2):
                out.writeframes(b''.join(struct.pack('<hh', x // scale, -x // (2 * scale))
                                        for x in cycle) * 3840)
    (root / 'index.html').write_text('<!doctype html><meta charset="utf-8"><script src="app.js"></script>', encoding='utf-8')
    (root / 'launch.lua').write_text(LUA, encoding='utf-8')
    (root / 'app.js').write_text(f'const checkContinuity = {json.dumps(check_continuity)};\n' + JS, encoding='utf-8')


LUA = r"""
local root=debug.getinfo(1,'S').source:sub(2):match('^(.*[/\\])')
local ok,err=xpcall(function()
  reaper.InsertTrackAtIndex(0,false)
  reaper.InsertTrackAtIndex(1,false)
  local target,other=reaper.GetTrack(0,0),reaper.GetTrack(0,1)
  for index, track in ipairs({target,other}) do
    for _,span in ipairs({{0,10},{11,9}}) do
      local item=reaper.AddMediaItemToTrack(track)
      local take=reaper.AddTakeToMediaItem(item)
      reaper.SetMediaItemTake_Source(take,reaper.PCM_Source_CreateFromFile(root..(index==1 and 'target.wav' or 'other.wav')))
      reaper.SetMediaItemInfo_Value(item,'D_POSITION',span[1])
      reaper.SetMediaItemInfo_Value(item,'D_LENGTH',span[2])
      reaper.SetMediaItemTakeInfo_Value(take,'D_STARTOFFS',span[1])
      reaper.SetMediaItemInfo_Value(item,'D_FADEINLEN',0)
      reaper.SetMediaItemInfo_Value(item,'D_FADEOUTLEN',0)
    end
  end
  reaper.SetMediaTrackInfo_Value(target,'D_VOL',0.5)
  reaper.SetMediaTrackInfo_Value(target,'I_PANMODE',3)
  reaper.SetMediaTrackInfo_Value(target,'D_PANLAW',1)
  reaper.SetMediaTrackInfo_Value(reaper.GetMasterTrack(0),'D_VOL',0.25)
  reaper.SetOnlyTrackSelected(target)
  local guid=assert(io.open(root..'track.txt','w'))
  guid:write(reaper.GetTrackGUID(target));guid:close()
  local id=reaper.ReaWeb_Open(root..'index.html')
  assert(id>0,reaper.ReaWeb_GetLastError())
  local commands={
    addFX=function() return reaper.TrackFX_AddByName(target,'JS: rwa_test_gain',false,-1) end,
    removeFX=function() return reaper.TrackFX_Delete(target,0) end,
    addReceive=function() return reaper.CreateTrackSend(other,target) end,
    removeReceive=function() return reaper.RemoveTrackSend(other,0,0) end,
    version=reaper.GetAppVersion,
    topology=function()
      local counts={reaper.CountTracks(0)}
      for i=-1,counts[1]-1 do
        local t=i==-1 and reaper.GetMasterTrack(0) or reaper.GetTrack(0,i)
        for _,kind in ipairs({-1,0,1}) do counts[#counts+1]=reaper.GetTrackNumSends(t,kind) end
        counts[#counts+1]=reaper.TrackFX_GetCount(t)
        counts[#counts+1]=reaper.TrackFX_GetRecCount(t)
      end
      return table.concat(counts,',')
    end,
    play=function() reaper.SetEditCurPos(0,false,false); reaper.OnPlayButton(); return true end,
    playing=reaper.GetPlayState,
    trackPeak=function() return reaper.Track_GetPeakInfo(target,0) end,
    position=reaper.GetPlayPosition2,
    realtime=function() return reaper.SetMediaTrackInfo_Value(target,'I_PERFFLAGS',2) end,
    lower=function() return reaper.SetMediaTrackInfo_Value(target,'D_VOL',0.25) end,
    panRight=function() return reaper.SetMediaTrackInfo_Value(target,'D_PAN',1) end,
    panCenter=function() return reaper.SetMediaTrackInfo_Value(target,'D_PAN',0) end,
    bypass=function() reaper.TrackFX_SetEnabled(target,0,false); return true end,
    seek=function() reaper.SetEditCurPos(12,false,true); return true end,
    seekGap=function() reaper.SetEditCurPos(10.25,false,true); return true end,
    rewind=function() reaper.SetEditCurPos(0,false,true); return true end,
    stop=function() reaper.OnStopButton(); return true end
  }
  local function poll()
    if not reaper.ReaWeb_IsOpen(id) then return end
    local message=reaper.ReaWeb_Receive(id)
    if commands[message] then reaper.ReaWeb_Send(id,tostring(commands[message]())) end
    reaper.defer(poll)
  end
  poll()
end,debug.traceback)
if not ok then local f=assert(io.open(root..'launcher-error.txt','w'));f:write(err);f:close() end
"""


JS = r"""
(async()=>{
 const checks={}, wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
 const assert=(ok,message)=>{if(!ok)throw Error(message);};
 const near=(value,expected)=>Math.abs(value-expected)<.006;
 const until=async(check,label)=>{
   for(let i=0;i<200;i++){const value=await check();if(value)return value;await wait(15);}
   throw Error('Timeout: '+label);
 };
 const peaks=stream=>{
   let packet;
   const values=[0,0];
   while((packet=stream.read()))for(let i=0;i<packet.data.length;i+=2){
     values[0]=Math.max(values[0],Math.abs(packet.data[i]));
     values[1]=Math.max(values[1],Math.abs(packet.data[i+1]));
   }
   return values;
 };
 const matches=(values,left)=>near(values[0],left)&&near(values[1],left/2);
 let streams=[];
 try {
   await reaper.lifecycle.ready;
   let reply;
   await reaper.events.on('message',value=>{if(reply)reply(value);});
   const call=async command=>{
     const response=new Promise(resolve=>{reply=resolve;});
     await reaper.host.send(command);
     return await Promise.race([response,wait(5000).then(()=>{throw Error('Lua timeout: '+command);})]);
   };
   const guid=(await reaper.fs.readText('track.txt')).trim();
   const caps=await call('version');
   const taps=['post-fx','post-fader'];
   async function checkPlaybackWindow(phase, multiplier) {
     if (!checkContinuity) return;
     const rate=48000, count=65536;
     assert(Number(checks.audioDevice.SRATE)===rate,'continuity check requires 48 kHz');
     const first=Math.round(Number(await call('position'))*rate)+rate/4;
     assert(first+count<10*rate,'continuity window must precede source level change');
     const results=taps.map(()=>({frames:0,duplicates:0,wrong:0}));
     const detach=taps.map((tap,k)=>{
       const seen=new Uint8Array(count), result=results[k], gain=k===0?multiplier:multiplier/2;
       return streams[k].on('data',packet=>{
         streams[k].read();
         const offset=Math.round(packet.timestamp*rate)-first;
         for(let i=Math.max(0,-offset);i<Math.min(packet.data.length/2,count-offset);i++){
           const j=offset+i;
           if(seen[j]) result.duplicates++;
           else { seen[j]=1; result.frames++; }
           const x=Math.trunc(4096*Math.sin(2*Math.PI*((first+j)%125)/125));
           if(!(Math.abs(packet.data[2*i]-gain*x/32768)<=1e-6 &&
                Math.abs(packet.data[2*i+1]-gain*Math.floor(-x/2)/32768)<=1e-6)) result.wrong++;
         }
       });
     });
     try {
       await until(async()=>Number(await call('position'))*rate>first+count+8192,phase);
       checks[phase]=results;
       assert(results.every(r=>r.frames===count && !r.duplicates && !r.wrong),phase+' PCM mismatch');
     } finally { detach.forEach(f=>f()); }
   }

   checks.version=caps;
   checks.audioDevice=(await reaper.system.getDevices()).audio;
   const topology=await call('topology');
   for(const tap of taps) {
     const stream=await reaper.audio.openStream('audio',{source:tap==='post-fx'?'selected-track':'track:'+guid,tap});
     streams.push(stream);
     assert(stream.info.source.endsWith(':'+tap),stream.info.source);
     stream.on('error',error=>checks[tap+'Error']=error.code);
   }
   const legacy=await reaper.audio.openStream('waveform',{source:'selected-track'});
   streams.push(legacy);
   assert(legacy.info.source.endsWith(':pre-fx'),'legacy accessor identity');
   const meter=await reaper.audio.openStream('meter',{source:'track:'+guid,tap:'post-fader',integratedMode:'continuous'});
   streams.push(meter);
   const spectrum=await reaper.audio.openStream('spectrum',{source:'track:'+guid,tap:'post-fader',fftSize:512});
   streams.push(spectrum);
   const waveform=await reaper.audio.openStream('waveform',{source:'track:'+guid,tap:'post-fader'});
   streams.push(waveform);
   assert(await call('topology')===topology,'opening track taps changed tracks, routing or FX');
   await call('play');
   await until(async()=>Number(await call('playing'))&1,'playback');
   checks.emptyTrackPeak=await until(async()=>{
     const peak=Number(await call('trackPeak'));
     return peak>.01 && peak;
   },'REAPER track peak with empty FX chain');
   await until(()=>{
     checks.empty=taps.map((_,i)=>peaks(streams[i]));
     return matches(checks.empty[0],.125)&&matches(checks.empty[1],.0625);
   },'empty FX chain PCM');
   await checkPlaybackWindow('emptyContinuity',1);
   await call('panRight');
   await until(()=>{
     checks.emptyPan=taps.map((_,i)=>peaks(streams[i]));
     const [pre,post]=checks.emptyPan;
     return pre[0]<.0001 && post[0]<.0001 && near(pre[1],.0625) && near(post[1],.03125);
   },'empty FX chain balance');
   await call('panCenter');
   assert(await call('addReceive')==='0','add test receive');
   await until(()=>{
     checks.emptyReceive=taps.map((_,i)=>peaks(streams[i]));
     return matches(checks.emptyReceive[0],.1875)&&matches(checks.emptyReceive[1],.09375);
   },'empty FX chain includes its receive');
   assert(await call('removeReceive')==='true','remove test receive');
   assert(await call('addFX')==='0','insert test gain');
   const baseline={'post-fx':.25,'post-fader':.125};
   await until(()=>{
     checks.peaks=Object.fromEntries(taps.map((tap,i)=>[tap,peaks(streams[i])]));
     return taps.every(tap=>matches(checks.peaks[tap],baseline[tap]));
   },'post-FX and post-fader PCM');
   await until(()=>near(meter.latest()?.data[0],.125)&&spectrum.latest()&&waveform.latest(),'analysis after fader');
   const post=streams[taps.indexOf('post-fader')];
   await checkPlaybackWindow('normalContinuity',2);
   peaks(post);
   await call('realtime');
   await checkPlaybackWindow('realtimeContinuity',2);
   await call('lower');
   await until(()=>{
     checks.fader=peaks(post);
     checks.beforeFader=peaks(streams[taps.indexOf('post-fx')]);
     return matches(checks.fader,.0625)&&matches(checks.beforeFader,.25);
   },'only post-fader changes with fader');
   await call('panRight');
   await until(()=>{
     checks.panned=peaks(post);
     checks.beforePan=peaks(streams[taps.indexOf('post-fx')]);
     // The parent mix receives channel balance before its final fader gain.
     return checks.panned[0]<.0001&&near(checks.panned[1],.03125)&&
       checks.beforePan[0]<.0001&&near(checks.beforePan[1],.125);
   },'right balance at the parent mix input and after the fader');
   await call('panCenter');
   await call('bypass');
   await until(()=>{
     checks.bypassed=peaks(post);
     checks.bypassedBeforeFader=peaks(streams[taps.indexOf('post-fx')]);
     return matches(checks.bypassed,.03125)&&matches(checks.bypassedBeforeFader,.125);
   },'FX bypass changes post-fader signal');
   const freshMeter=async(sequence,label)=>await until(()=>{
     const packet=meter.latest();
     return packet&&packet.sequence>sequence&&near(packet.data[0],.03125);
   },label);
   const beforeReset=meter.latest().sequence;
   await reaper.audio.resetMeter(meter.info.name);
   await freshMeter(beforeReset,'meter reset on live source');
   await call('seek');
   await until(()=>{
     checks.afterSeek=taps.map((_,i)=>peaks(streams[i]));
     return matches(checks.afterSeek[0],.0625)&&matches(checks.afterSeek[1],.015625);
   },'new source PCM after seeking');
   assert(await call('removeFX')==='true','remove last FX');
   await call('rewind');
   await until(()=>{
     checks.afterRemoval=taps.map((_,i)=>peaks(streams[i]));
     return matches(checks.afterRemoval[0],.125)&&matches(checks.afterRemoval[1],.03125);
   },'new PCM after removing the last FX');
   await call('seekGap');
   await until(async()=>Number(await call('position'))>=10.5,'seek to silent gap');
   for (const stream of streams.slice(0,2)) {
     let packet;
     while ((packet=stream.read())) {
       if (packet.timestamp>=10.25 && packet.timestamp<11)
         assert(packet.data.every(sample=>Math.abs(sample)<.0001),'stale PCM in silent gap');
     }
   }
   await call('rewind');
   const beforeDetach=meter.latest().sequence;
   await post.close();
   await until(()=>{
     const packet=meter.latest();
     return packet&&packet.sequence>beforeDetach&&near(packet.data[0],.03125);
   },'shared capture survives detach');
   await Promise.all(streams.map(stream=>stream.close()));
   assert(await call('topology')===topology,'closing track taps changed tracks, routing or FX');
   await call('stop');
   await reaper.fs.writeText('result.json',JSON.stringify({passed:true,checks}));
 } catch(error) {
   await reaper.fs.writeText('result.json',JSON.stringify({passed:false,checks,error:String(error),stack:error.stack}));
 } finally { await Promise.allSettled(streams.map(stream=>stream.close())); }
})();
"""


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('reaper', 'extension', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--check-continuity', action='store_true', help='Verify both stereo taps frame by frame at 48 kHz')
    args = parser.parse_args()
    root = args.output.resolve()
    prepare(root, args.extension, args.check_continuity)
    command = [str(args.reaper.resolve()), '-newinst', '-cfgfile', str(root / 'reaper.ini'),
               '-new', '-ignoreerrors', '-nosplash', str(root / 'launch.lua')]
    if args.prepare_only:
        print(subprocess.list2cmdline(command) if sys.platform == 'win32' else command)
        sys.exit(0)
    process = subprocess.Popen(command)
    try:
        deadline = time.monotonic() + 55
        result = root / 'result.json'
        launcher_error = root / 'launcher-error.txt'
        while not result.exists() and not launcher_error.exists() and process.poll() is None and time.monotonic() < deadline:
            time.sleep(.15)
        if not result.exists():
            raise RuntimeError(launcher_error.read_text() if launcher_error.exists() else 'No REAPER result')
        payload = json.loads(result.read_text(encoding='utf-8'))
        print(json.dumps(payload, indent=2))
        sys.exit(0 if payload['passed'] else 1)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
