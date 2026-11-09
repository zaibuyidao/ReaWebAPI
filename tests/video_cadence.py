"""Opt-in video cadence acceptance with a native 60 FPS, 12-bit barcode fixture."""
import argparse
import configparser
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

PAGE = r'''<!doctype html><meta charset="utf-8"><title>Video cadence</title><canvas width="1280" height="720" style="width:1280px;height:720px"></canvas><script type="module">
import { createFrameRenderer } from './frame-renderer.js';
(async()=>{
 const minimum=__MINIMUM__,maxSkipped=__MAX_SKIPPED__,modes=__PHASES__;
 const rendererMode=__RENDERER__,continuous=rendererMode==='continuous',barcode=__BARCODE__;
 const result={phases:[],renderer:rendererMode,smokeOnly:minimum===null&&maxSkipped===null,minimumUniqueFPS:minimum,maxSkippedFrames:maxSkipped,identity:barcode?'barcode':'sampledPixelHash'};let stream,renderer,raf=0;
 const sleep=ms=>new Promise(r=>setTimeout(r,ms));
 try {
  await reaper.lifecycle.ready;await reaper.SetExtState('VideoCadence','progress','ready',false);await reaper.window.show();await reaper.SetExtState('VideoCadence','progress','shown',false);await reaper.window.focus();await reaper.SetExtState('VideoCadence','progress','focused',false);
  const duration=Number(await reaper.GetExtState('VideoCadence','duration'));
  result.audioDevice={};for(const key of ['MODE','SRATE','BSIZE'])result.audioDevice[key]=await reaper.GetAudioDeviceInfo(key);
  const probe=(await reaper.GetExtState('CadenceProbe','available'))==='1';result.nativeProbe=probe;
  if(probe){
   await reaper.SetExtState('CadenceProbe','prepared','',false);
   await reaper.SetExtState('CadenceProbe','request','prepare',false);
   const deadline=performance.now()+5000;let prepared='';
   while(prepared===''&&performance.now()<deadline){await sleep(20);prepared=await reaper.GetExtState('CadenceProbe','prepared');}
   if(prepared!=='1')throw Error('Native cadence probe could not verify the video module');
  }
  const canvas=document.querySelector('canvas'),ctx=canvas.getContext('2d');
  const stats=(ids)=>{let unique=new Set(ids),changes=0,skipped=0,previous;
   for(const id of ids){if(previous!==undefined&&id!==previous){++changes;const d=(id-previous+4096)%4096;if(barcode&&d<120)skipped+=Math.max(0,d-1);}previous=id;}
   return {samples:ids.length,distinct:unique.size,changes,skipped,first:ids[0],last:ids.at(-1)};};
  for(const mode of modes) {
   await reaper.SetExtState('VideoCadence','progress',mode,false);
   await reaper.OnStopButton();await reaper.SetEditCurPos(1,false,true);
   const visible=(await reaper.GetToggleCommandState(50125))===1;
   if(visible!==(mode!=='app'))await reaper.Main_OnCommand(50125,0);
   let pending,record=false,received=[],painted=[],sequences=[],paintedSequences=[],times=[],ticks=[],receiveTicks=[],refreshTicks=[],paintAges=[],longTasks=[],arrivals=new WeakMap(),refresh=0;
   const refreshTick=t=>{if(record)refreshTicks.push(t);refresh=requestAnimationFrame(refreshTick);};
   const observer=PerformanceObserver.supportedEntryTypes?.includes('longtask')?new PerformanceObserver(list=>{if(record)for(const entry of list.getEntries())longTasks.push({start:entry.startTime,duration:entry.duration});}):null;
   observer?.observe({entryTypes:['longtask']});
   const decode=frame=>{if(!barcode){let hash=2166136261;for(let i=0;i<frame.bytes.length;i+=4093)hash=Math.imul(hash^frame.bytes[i],16777619);return hash>>>0;}let id=0;for(let i=0;i<12;i++)if(frame.bytes[(24*1280+24+i*20)*4]>128)id|=1<<i;return id;};
   const paint=f=>{ctx.putImageData(new ImageData(new Uint8ClampedArray(f.bytes.buffer,f.bytes.byteOffset,f.bytes.byteLength),1280,720),0,0);if(record){painted.push(decode(f));paintedSequences.push(Number(f.sequence));ticks.push(performance.now());paintAges.push(performance.now()-arrivals.get(f));}};
   const draw=()=>{raf=0;if(continuous)raf=requestAnimationFrame(draw);if(!pending)return;const f=pending;pending=null;paint(f);};
   if(mode!=='native') {
    refresh=requestAnimationFrame(refreshTick);
    if(probe)await reaper.SetExtState('CadenceProbe','request','show',false);
    await reaper.window.setPosition(100,100);await reaper.window.setSize(1296,760);await reaper.window.show();await reaper.window.focus();
    const visibleDeadline=performance.now()+5000;while(document.hidden&&performance.now()<visibleDeadline)await sleep(50);
    if(document.hidden)throw Error('Preview page remains hidden');
    stream=await reaper.video.openStream({width:1280,height:720,updateRate:'auto'});
    if(rendererMode==='buffered')renderer=createFrameRenderer(paint);
    stream.on('data',f=>{arrivals.set(f,performance.now());if(record){received.push(decode(f));sequences.push(Number(f.sequence));times.push(f.timestamp);receiveTicks.push(performance.now());}if(renderer){renderer.push(f);return;}pending=f;if(!raf)raf=requestAnimationFrame(draw);});
    const clear=()=>{renderer?.clear();pending=null;if(raf)cancelAnimationFrame(raf);raf=0;ctx.clearRect(0,0,canvas.width,canvas.height);};
    stream.on('error',clear);stream.on('close',clear);
    if(continuous)raf=requestAnimationFrame(draw);
   }
   await reaper.OnPlayButton();await sleep(__WARMUP__*1000);
   const before=await reaper.video.getDiagnostics();
   if(probe)await reaper.SetExtState('CadenceProbe','request','start',false);
   const dropsBefore=renderer?.dropped||0;
   const begin=performance.now();record=true;await sleep(duration*1000);record=false;
   const rendererDrops=(renderer?.dropped||0)-dropsBefore;
   const measured={received:received.slice(),painted:painted.slice(),sequences:sequences.slice(),paintedSequences:paintedSequences.slice(),times:times.slice(),ticks:ticks.slice(),receiveTicks:receiveTicks.slice(),refreshTicks:refreshTicks.slice(),paintAges:paintAges.slice(),longTasks:longTasks.slice()};
   if(refresh)cancelAnimationFrame(refresh);
   const elapsed=(performance.now()-begin)/1000,after=await reaper.video.getDiagnostics();
   observer?.disconnect();
   let official=null;if(probe){await reaper.SetExtState('CadenceProbe','request','stop',false);await sleep(100);official=JSON.parse(await reaper.GetExtState('CadenceProbe','result'));}
   {
   const {received,painted,sequences,paintedSequences,times,ticks,receiveTicks,refreshTicks,paintAges,longTasks}=measured;
   const phase={mode,seconds:elapsed,received:stats(received),painted:stats(painted),receivedFPS:received.length/elapsed,paintedFPS:painted.length/elapsed,uniqueReceivedFPS:stats(received).changes/elapsed,uniquePaintedFPS:stats(painted).changes/elapsed,captured:after.capturedFrames-before.capturedFrames,capturedFPS:(after.capturedFrames-before.capturedFrames)/elapsed,target:after.targetUpdateRate,firstTimestamp:times[0],lastTimestamp:times.at(-1),maxPaintGapMs:Math.max(0,...ticks.slice(1).map((v,i)=>v-ticks[i])),official,checks:{}};
   for(const key of ['captureFailures','droppedFrames','transportLatencyMs','captureMs'])phase[key]=after[key];
   phase.transportSequenceGaps=sequences.slice(1).reduce((n,v,i)=>n+Math.max(0,v-sequences[i]-1),0);
   phase.longTasks=longTasks.map(t=>({startMs:t.start-begin,durationMs:t.duration}));
   const gaps=values=>values.flatMap((v,i)=>i&&v-values[i-1]>25?[{atMs:v-begin,gapMs:v-values[i-1]}]:[]);
   phase.receiveGaps=gaps(receiveTicks);phase.paintGaps=gaps(ticks);
   phase.receiveSpanSeconds=(receiveTicks.at(-1)-receiveTicks[0])/1000;
   phase.refreshFPS=refreshTicks.length/elapsed;
   phase.refreshSpanFPS=(refreshTicks.length-1)*1000/(refreshTicks.at(-1)-refreshTicks[0]);
   phase.firstSequence=sequences[0];phase.lastSequence=sequences.at(-1);
   phase.beforeCaptured=before.capturedFrames;phase.afterCaptured=after.capturedFrames;
   phase.firstReceiveOffsetMs=receiveTicks[0]-begin;phase.lastReceiveOffsetMs=receiveTicks.at(-1)-begin;
   const drawn=new Set(paintedSequences);
   phase.receivedButNotPainted=sequences.filter(id=>id>paintedSequences[0]&&id<paintedSequences.at(-1)&&!drawn.has(id));
   phase.rendererDrops=rendererDrops;phase.maxPaintAgeMs=Math.max(0,...paintAges);phase.meanPaintAgeMs=paintAges.reduce((a,b)=>a+b,0)/Math.max(1,paintAges.length);
   phase.clearedFrames=after.clearedFrames-before.clearedFrames;
   if(barcode&&official?.frameIds&&received.length&&received[0]<received.at(-1)) {
    const seen=new Set(received);
    phase.observedButNotReceived=official.frameIds.filter(id=>id>received[0]&&id<received.at(-1)&&!seen.has(id));
   }
   if(mode!=='native') {
    for(const metric of ['capturedFPS','receivedFPS','paintedFPS','uniqueReceivedFPS','uniquePaintedFPS'])
     phase.checks[metric]={actual:phase[metric],minimum,passed:minimum===null?null:Number.isFinite(phase[metric])&&phase[metric]>=minimum};
    phase.checks.smokeFrames={actual:phase.painted.distinct,minimumExclusive:30,passed:phase.painted.distinct>30};
    phase.ok=minimum===null?phase.checks.smokeFrames.passed:Object.entries(phase.checks).filter(([name])=>name!=='smokeFrames').every(([,check])=>check.passed);
    if(maxSkipped!==null){phase.checks.skippedFrames={maximum:maxSkipped,passed:phase.received.skipped<=maxSkipped&&phase.painted.skipped<=maxSkipped&&phase.transportSequenceGaps<=maxSkipped&&phase.rendererDrops<=maxSkipped&&phase.clearedFrames===0};phase.ok&&=phase.checks.skippedFrames.passed;}
   } else {
    phase.checks.nativeObservation={passed:official? !official.error&&official.samples>0&&official.changedFrames>0:null};
    phase.ok=phase.checks.nativeObservation.passed;
   }
   result.phases.push(phase);
   }
   await reaper.OnStopButton();await stream?.close();stream=null;renderer?.clear();renderer=null;if(raf)cancelAnimationFrame(raf);raf=0;await sleep(200);
  }
  const appPhases=result.phases.filter(p=>p.mode!=='native');
  result.ok=appPhases.length?appPhases.every(p=>p.ok)&&result.phases.filter(p=>p.mode==='native'&&probe).every(p=>p.ok):result.phases.every(p=>p.ok===true);
 }catch(e){result.ok=false;result.error=String(e);result.stack=e.stack;}
 finally{renderer?.clear();if(raf)cancelAnimationFrame(raf);await stream?.close().catch(()=>{});}
 await reaper.SetExtState('VideoCadence','result',JSON.stringify(result),false);
})();</script>'''
RUNNER = r'''
local r=reaper
local root=r.GetResourcePath()
local function write(name,text)local f=assert(io.open(root..'/'..name,'w'));f:write(text);f:close()end
local function main()
 assert(r.APIExists('ReaWeb_Open'),'extension not loaded')
 r.SetExtState('VideoCadence','duration','__DURATION__',false)
 write('setup.txt','before insert')
 r.GetSetProjectInfo(0,'PROJECT_SRATE',48000,true)
 r.InsertMedia(root..'/motion.mp4',1)
 write('setup.txt','inserted')
 assert(r.GetMediaItem(0,0),'media import failed')
 r.Audio_Init()
 write('setup.txt','audio initialized')
 local id=r.ReaWeb_Open(root..'/app/index.html');assert(id>0,r.ReaWeb_GetLastError())
 write('setup.txt','opened')
 local begin=r.time_precise()
 local last_progress=''
 local function poll()
  local progress=r.GetExtState('VideoCadence','progress')
  if progress~=last_progress then last_progress=progress;write('progress.txt',progress)end
  local result=r.GetExtState('VideoCadence','result')
  if result~='' then write('result.json',result);write('diagnostics.json',r.ReaWeb_GetDiagnostics(id));r.ReaWeb_Close(id);r.OnStopButton();if r.GetToggleCommandState(50125)==1 then r.Main_OnCommand(50125,0)end;return end
  if r.time_precise()-begin>__TIMEOUT__ then write('error.txt','Page timeout');return end
  r.defer(poll)
 end
 r.defer(poll)
end
local ok,err=xpcall(main,debug.traceback);if not ok then write('error.txt',err)end
'''

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for key in ('profile','reaper','extension','media'):p.add_argument('--'+key,type=Path,required=True)
 p.add_argument('--duration',type=int,default=10)
 p.add_argument('--warmup',type=float,default=1.5)
 p.add_argument('--native-probe',type=Path)
 p.add_argument('--resource-source',type=Path,help='Resource directory supplying license and macOS audio configuration')
 p.add_argument('--minimum-unique-fps',type=float,help='Require capture, receive, paint and unique-frame rates to meet this floor; omitted runs are smoke tests')
 p.add_argument('--max-skipped-frames',type=int,help='Maximum numbered-frame and transport sequence gaps per phase')
 p.add_argument('--phases',default='native,app,both',help='Comma-separated phases: native,app,both')
 p.add_argument('--project-fps',type=float,default=60)
 p.add_argument('--renderer',choices=('event','continuous','buffered'),default='buffered')
 p.add_argument('--linux-audio',choices=('pulse','dummy'),default='pulse',help='Audio clock for the disposable Linux profile')
 p.add_argument('--continuous-raf',action='store_true',help='Legacy alias selecting the continuous renderer')
 p.add_argument('--no-barcode',action='store_true')
 args=p.parse_args()
 phases=args.phases.split(',')
 if not phases or any(mode not in ('native','app','both') for mode in phases) or len(set(phases))!=len(phases):p.error('--phases must contain distinct native, app or both entries')
 if args.duration<=0 or not math.isfinite(args.project_fps) or args.project_fps<=0:p.error('Duration and project FPS must be positive')
 if not math.isfinite(args.warmup) or args.warmup<0:p.error('--warmup must be finite and nonnegative')
 if args.minimum_unique_fps is not None and (not math.isfinite(args.minimum_unique_fps) or args.minimum_unique_fps<=0):p.error('--minimum-unique-fps must be finite and positive')
 if args.max_skipped_frames is not None and (args.max_skipped_frames<0 or args.no_barcode):p.error('--max-skipped-frames requires barcode input and a nonnegative limit')
 if phases==['native'] and not args.native_probe:p.error('--phases native requires --native-probe')
 if phases==['native'] and args.minimum_unique_fps is not None:p.error('--minimum-unique-fps requires an app or both phase')
 root=args.profile.resolve();root.mkdir(parents=True,exist_ok=False)
 for name in ('UserPlugins','EmptyPlugins','app'): (root/name).mkdir()
 shutil.copy2(args.extension,root/'UserPlugins'/args.extension.name)
 if args.native_probe:shutil.copy2(args.native_probe,root/'UserPlugins'/args.native_probe.name)
 source=args.reaper.resolve().parent;exe=args.reaper.resolve();resources=args.resource_source.resolve() if args.resource_source else source
 config='[reaper]\nloadlastproj=0\nshowlastproj=0\nsplash=0\nverchk=0\nerrnowarn=5\nhasrecentlyopened=1\nnewprojdo=0\nvstfullstate=58181\nvstpath64=\nvstpath=\nprojfrbase=60\nprojfrdrop=0\n'
 config=config.replace('projfrbase=60',f'projfrbase={round(args.project_fps)}').replace('projfrdrop=0',f'projfrdrop={int(args.project_fps!=round(args.project_fps))}')
 if sys.platform=='win32':
  exe=root/'reaper.exe';shutil.copy2(args.reaper,exe);(root/'Plugins').mkdir()
  for name in ('reaper_video.dll','reaper_wave.dll','FFmpeg.dll'):shutil.copy2(source/'Plugins'/name,root/'Plugins'/name)
  config+='clap_path_win64='+str(root/'EmptyPlugins')+'\n[audioconfig]\nmode=4\ndummy_srate=48000\ndummy_blocksize=256\nwaveout_devicein=-1\nwaveout_deviceout=0\nwaveout_srate=48000\nwaveout_bps=16\nwaveout_bs=256\nwaveout_numblocks=4\nwaveout_nch_out=2\n'
 elif sys.platform=='darwin':
  resources=args.resource_source.resolve() if args.resource_source else Path.home()/'Library/Application Support/REAPER'
  previous=configparser.RawConfigParser(strict=False);previous.read(resources/'reaper.ini')
  if previous.has_section('reaper'):config+=''.join(k+'='+v+'\n' for k,v in previous['reaper'].items() if k.startswith('coreaudio'))
  for path in (root/'UserPlugins').iterdir():subprocess.run(['codesign','--force','--sign','-',str(path)],check=True)
 else:
  config+=f'linux_audio_mode={2 if args.linux_audio=="dummy" else 3}\nlinux_audio_srate=48000\nlinux_audio_bsize=256\nlinux_audio_nch_out=2\n'
  helper=args.extension.parent/args.extension.name.replace('reaper_reawebapi-','reawebapi-webview-').removesuffix('.so');shutil.copy2(helper,root/'UserPlugins'/helper.name)
 for name in ('reaper-license.rk','reaper-install-rev.txt'):
  if (resources/name).exists():shutil.copy2(resources/name,root/name)
 shutil.copy2(args.media,root/'motion.mp4')
 (root/'reaper.ini').write_text(config+'[reaper_video]\nvisible=0\n')
 (root/'app/app.json').write_text('{"id":"video-cadence","entry":"index.html"}')
 shutil.copy2(Path(__file__).resolve().parents[1]/'examples/integrations/video-preview/frame-renderer.js',root/'app/frame-renderer.js')
 (root/'app/index.html').write_text(PAGE.replace('__RENDERER__',json.dumps('continuous' if args.continuous_raf else args.renderer)).replace('__BARCODE__',str(not args.no_barcode).lower()).replace('__MINIMUM__',json.dumps(args.minimum_unique_fps)).replace('__MAX_SKIPPED__',json.dumps(args.max_skipped_frames)).replace('__WARMUP__',str(args.warmup)).replace('__PHASES__',json.dumps(phases)),encoding='utf-8')
 timeout=90+math.ceil(args.duration+args.warmup)*len(phases)
 (root/'runner.lua').write_text(RUNNER.replace('__DURATION__',str(args.duration)).replace('__TIMEOUT__',str(timeout-5)))
 env=os.environ.copy();env['CLAP_PATH']=str(root/'EmptyPlugins');env['REAWEB_CADENCE_PROBE']='1'
 with (root/'process.log').open('w') as log:
  proc=subprocess.Popen([str(exe),'-newinst','-cfgfile',str(root/'reaper.ini'),'-new','-ignoreerrors','-nosplash',str(root/'runner.lua')],cwd=root,env=env,stdout=log,stderr=log)
  try:
   for _ in range(timeout*2):
    if (root/'result.json').exists() or (root/'error.txt').exists() or proc.poll() is not None:break
    time.sleep(.5)
  finally:
   if proc.poll() is None:
    proc.terminate()
    try:proc.wait(timeout=5)
    except subprocess.TimeoutExpired:proc.kill();proc.wait()
 if (root/'error.txt').exists():raise RuntimeError((root/'error.txt').read_text())
 result=json.loads((root/'result.json').read_text());print(json.dumps(result,indent=2))
 if not result['ok']:raise SystemExit(1)
if __name__=='__main__':main()
