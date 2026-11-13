"""Opt-in Windows REAPER acceptance for mixed-resolution video without reopening streams."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
SIZES = [(1280, 720), (1920, 1080), (3840, 2160), (720, 1280)]
ORDER = [0, 1, 2, 3, 2, 0]
MODES = ('native-source', 'native-i420', 'fit', 'web-source', 'worker-source', 'worker-i420', 'custom')

PAGE = r'''<!doctype html><meta charset="utf-8"><title>Dynamic video acceptance</title>
<style>html,body{margin:0;background:transparent;color:white}#label{position:absolute;z-index:2;background:#222;padding:8px}canvas{display:block;width:640px;height:480px;object-fit:contain}</style>
<div id="label">Preparing</div><div id="stage"></div><script type="module">
import { createCanvasRenderer } from './canvas-renderer.js';
import { createVideoRenderer } from './video-renderer.js';
const modes=__MODES__,sizes=__SIZES__,order=__ORDER__,clipSeconds=__SECONDS__;
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const check=(condition,message)=>{if(!condition)throw Error(message);};
const wait=async(fn,label,timeout=12000)=>{const end=performance.now()+timeout;while(performance.now()<end){const value=await fn();if(value)return value;await sleep(40);}throw Error('Timeout: '+label);};
const result={ok:false,phases:[]};let consumer,painter,worker;
const connectionCount=async()=>{const d=await reaper.stream.getDiagnostics();return d.transport?.connections??d.connections;};
try {
 await reaper.lifecycle.ready;await reaper.window.show();await reaper.window.focus();
 await wait(()=>!document.hidden,'visible page');
 await reaper.window.setSize(680,540);
 result.capabilities=await reaper.video.getCapabilities();
 for(const mode of modes){
  document.querySelector('#label').textContent=mode;
  await reaper.SetExtState('VideoDynamic','progress',mode,false);
  await reaper.OnStopButton();await reaper.SetEditCurPos(0.2,false,true);
  await wait(async()=>{const s=await reaper.video.getSourceInfo();result.initialSource=s;return s.available&&s.width===sizes[0][0];},'initial source');
  const native=mode.startsWith('native-'),custom=mode==='custom',workerMode=mode.startsWith('worker-'),i420=mode.endsWith('i420');
  const format=i420?'i420':'rgba8';
  const options=custom?{resolution:'custom',width:640,height:360,format}:mode==='fit'?{resolution:'fit',width:640,height:480,format}:{resolution:'source',format};
  const phase={mode,options,latencyScope:native?'RPC-observed native submission, not screen presentation':'Target-size frame delivery and renderer callback, not screen presentation',closed:[],errors:[],dimensions:[],frames:0,drawn:0,maxGapMs:0,samples:[]};
  result.phases.push(phase);
  const canvas=document.createElement('canvas');document.querySelector('#stage').replaceChildren(canvas);canvas.hidden=native;
  let lastArrival=0,lastSequence=-1n,observing=true,seek=null;
  const queuedFrames=new Map();
  const matches=frame=>seek&&(!seek.verifySource||(seek.minimumSequence!==undefined&&frame.sequence>=seek.minimumSequence))&&Math.abs(frame.timestamp-seek.position)<0.045&&frame.width===seek.width&&frame.height===seek.height;
  const drawn=frame=>{++phase.drawn;if(matches(frame)&&seek.drawMs===undefined)seek.drawMs=performance.now()-seek.started;};
  if(workerMode){worker=await createVideoRenderer(canvas,{platform:'windows',format,onDraw:sequence=>{const f=queuedFrames.get(sequence);if(f)drawn(f);for(const id of queuedFrames.keys())if(id<=sequence)queuedFrames.delete(id);},onError:e=>phase.errors.push(String(e))});check(worker.openStream,'Worker stream unavailable');}
  else if(!native)painter=createCanvasRenderer(canvas);
  const end=performance.now()+12000;
  for(;;){try{consumer=native?await reaper.video.openSurface(options):workerMode?await worker.openStream(options):await reaper.video.openStream(options);break;}catch(e){if(i420&&e.code==='VIDEO_FORMAT_UNSUPPORTED'){phase.skipped=e.code;phase.sourceInfo=await reaper.video.getSourceInfo();break;}if(!['VIDEO_FRAME_PENDING','VIDEO_SOURCE_UNAVAILABLE'].includes(e.code)||performance.now()>=end)throw e;await sleep(80);}}
  if(!consumer){worker?.dispose();worker=null;continue;}
  phase.info=consumer.info;phase.name=consumer.info.name;
  check(!!phase.name,'Missing stream identity');
  if(native)await consumer.setLayout({rect:{x:0,y:0,width:640,height:480},clip:{x:0,y:0,width:640,height:480},preserveAspect:true});
  else if(!workerMode){canvas.width=consumer.info.width;canvas.height=consumer.info.height;}
  consumer.on('close',e=>{if(observing)phase.closed.push(e.code);});consumer.on('error',e=>{if(observing)phase.errors.push(e.code||String(e));});
  if(!native)consumer.on('data',frame=>{
   try{
    const width=frame.width||consumer.info.width,height=frame.height||consumer.info.height,stride=frame.stride||consumer.info.stride;
    check(width>0&&height>0&&stride===width*(i420?1:4),'Invalid frame layout');
    check(frame.sequence>lastSequence,'Non-monotonic frame sequence');lastSequence=frame.sequence;
    if(frame.bytes)check(frame.bytes.byteLength===stride*height*(i420?1.5:1),'Frame byte count does not match dimensions');
    if(frame.discontinuity){if(seek)++seek.clearPackets;return;}
    const metadata={width,height,timestamp:frame.timestamp,sequence:frame.sequence};
    if(seek){if(matches(metadata)&&seek.arrivalMs===undefined)seek.arrivalMs=performance.now()-seek.started;else if(!matches(metadata))++seek.stalePackets;}
    if(workerMode){queuedFrames.set(frame.sequence,metadata);if(queuedFrames.size>100)queuedFrames.delete(queuedFrames.keys().next().value);}
    const key=width+'x'+height;if(phase.dimensions.at(-1)!==key)phase.dimensions.push(key);
    const now=performance.now();if(lastArrival)phase.maxGapMs=Math.max(phase.maxGapMs,now-lastArrival);lastArrival=now;++phase.frames;
    if(painter){check(painter.draw(frame),'Web renderer failed');drawn(metadata);}
   }catch(e){phase.errors.push(String(e));}
  });
  const connectionsBefore=await connectionCount();phase.connectionsBefore=connectionsBefore;
  const names=new Set(),sourceSizes=new Set();
  await reaper.OnPlayButton();const begin=performance.now();
  while(performance.now()-begin<(order.length*clipSeconds+4)*1000){
   await sleep(90);
   const state=await reaper.video.getDiagnostics();names.add(state.stream);
   check(state.stream===phase.name,'Stream identity changed during playback');check(!consumer.closed,'Consumer closed during playback');
   const position=await reaper.GetPlayPosition();
   if(state.sourceWidth>0)sourceSizes.add(state.sourceWidth+'x'+state.sourceHeight);
   const sample={position,width:state.width,height:state.height,sourceWidth:state.sourceWidth,sourceHeight:state.sourceHeight,captured:state.capturedFrames,produced:state.producedFrames,format:state.format,colorConversion:state.colorConversion};
   if(native){
    const d=await consumer.getDiagnostics();sample.native=d;
    if(d.hasFrame){const key=d.sourceWidth+'x'+d.sourceHeight;if(phase.dimensions.at(-1)!==key)phase.dimensions.push(key);}
    phase.frames=d.sourceUploadedFrames;phase.drawn=d.submittedFrames;
   }
   phase.samples.push(sample);
   if(position>=order.length*clipSeconds-0.25)break;
  }
  await reaper.OnStopButton();await sleep(150);
  phase.connectionsAfter=await connectionCount();phase.sourceSizes=[...sourceSizes];phase.names=[...names];
  check(phase.errors.length===0,'Consumer/render errors: '+phase.errors.join(','));check(phase.closed.length===0,'Unexpected stream close');
  check(phase.connectionsAfter===connectionsBefore,'Socket connection count changed during playback');
  check(sizes.every(([w,h])=>sourceSizes.has(w+'x'+h)),'Not all source dimensions observed');
  const expected=custom?['640x360']:mode==='fit'?['640x360','270x480']:sizes.map(([w,h])=>w+'x'+h);
  check(expected.every(size=>phase.dimensions.includes(size)),'Missing output dimensions: '+expected+' observed '+phase.dimensions);
  check(custom?phase.dimensions.length===1:phase.dimensions.length>=3,'Insufficient output transitions');
  check(phase.frames>20&&phase.drawn>20,'Insufficient frames after size changes');
  phase.seeks=[];
  const targetDimensions=index=>{
   const [w,h]=sizes[index];if(custom)return[640,360];if(mode!=='fit')return[w,h];
   const scale=Math.min(1,640/w,480/h);return[Math.floor(w*scale),Math.floor(h*scale)];
  };
  const settle=async()=>{
   let targetSequence;
   seek.stateRpcMs=[];seek.nativeRpcMs=[];seek.captureSamples=[];
   const deadline=performance.now()+8000;
   while(performance.now()<deadline){
    check(!consumer.closed,'Consumer closed while seeking');
    if(native||seek.verifySource){
     const stateStart=performance.now(),state=await reaper.video.getDiagnostics();seek.stateRpcMs.push(performance.now()-stateStart);check(state.stream===phase.name,'Stream identity changed while seeking');
     seek.captureSamples.push({atMs:performance.now()-seek.started,position:state.position,status:state.status,width:state.width,height:state.height,captured:state.capturedFrames,produced:state.producedFrames,captureMs:state.captureMs,transportLatencyMs:state.transportLatencyMs});
     if(Math.abs(state.position-seek.position)<0.045&&state.width===seek.width&&state.height===seek.height&&state.sourceWidth===seek.sourceWidth&&state.sourceHeight===seek.sourceHeight&&state.status==='ready'){
      if(targetSequence===undefined){targetSequence=state.producedFrames+state.droppedFrames;seek.minimumSequence=BigInt(targetSequence);if(native)seek.arrivalMs=performance.now()-seek.started;}
      if(native){
       const nativeStart=performance.now(),d=await consumer.getDiagnostics();seek.nativeRpcMs.push(performance.now()-nativeStart);
       seek.nativeAfter=d;
       if(!d.hasFrame)++seek.emptySamples;
       if(d.hasFrame&&d.sequence>=targetSequence&&d.sourceWidth===seek.width&&d.sourceHeight===seek.height){seek.drawMs=performance.now()-seek.started;break;}
      }else if(seek.drawMs!==undefined)break;
     }
    }else if(seek.drawMs!==undefined)break;
    await sleep(10);
   }
   check(seek.drawMs!==undefined,'Target frame did not render after seek '+seek.position);
   seek.elapsedMs=performance.now()-seek.started;delete seek.started;delete seek.minimumSequence;
   const state=await reaper.video.getDiagnostics();seek.backendClears=state.clearedFrames-seek.clearsBefore;delete seek.clearsBefore;seek.captureAfter={captureMs:state.captureMs,produced:state.producedFrames,dropped:state.droppedFrames,transportLatencyMs:state.transportLatencyMs};
   check(state.stream===phase.name,'Stream identity changed while seeking');
   phase.seeks.push(seek);seek=null;
  };
  const seekOrder=[0,2,1,3,2,0,3,1,2,3,0,1,3,2,0,2,1,3,0,3,1,2,3,0];
  const plans=seekOrder.map((index,n)=>({index,offset:0.25+(n%5)*0.04,kind:'cross-size'}));
  for(const plan of plans){
   const {index,offset,kind}=plan,[width,height]=targetDimensions(index),state=await reaper.video.getDiagnostics(),nativeBefore=native?await consumer.getDiagnostics():null;
   seek={kind,position:index*clipSeconds+offset,width,height,sourceWidth:sizes[index][0],sourceHeight:sizes[index][1],verifySource:custom||mode==='fit',clearPackets:0,stalePackets:0,emptySamples:0,clearsBefore:state.clearedFrames,captureBefore:{captureMs:state.captureMs,produced:state.producedFrames,dropped:state.droppedFrames,transportLatencyMs:state.transportLatencyMs},nativeBefore,started:performance.now()};
   await reaper.SetEditCurPos(seek.position,false,true);seek.commandMs=performance.now()-seek.started;await settle();
  }
  const burstBegin=performance.now();
  for(let n=0;n<24;++n){await reaper.SetEditCurPos((n%4)*clipSeconds+0.3,false,true);await sleep(15);}
  const [bw,bh]=targetDimensions(0),beforeBurst=await reaper.video.getDiagnostics();
  seek={position:0.55,width:bw,height:bh,sourceWidth:sizes[0][0],sourceHeight:sizes[0][1],verifySource:custom||mode==='fit',burstCommands:24,burstDurationMs:performance.now()-burstBegin,clearPackets:0,stalePackets:0,emptySamples:0,clearsBefore:beforeBurst.clearedFrames,started:performance.now()};
  await reaper.SetEditCurPos(seek.position,false,true);seek.commandMs=performance.now()-seek.started;await settle();
  phase.connectionsAfterSeeks=await connectionCount();
  check(phase.connectionsAfterSeeks===connectionsBefore,'Socket connection count changed during rapid seeking');
  check(phase.closed.length===0&&phase.errors.length===0,'Consumer/render error during seeking');
  const latencies=phase.seeks.filter(s=>s.kind==='cross-size').map(s=>s.drawMs).sort((a,b)=>a-b);
  phase.seekSummary={count:latencies.length,medianDrawMs:latencies[Math.floor(latencies.length/2)],p95DrawMs:latencies[Math.ceil(latencies.length*.95)-1],maxDrawMs:latencies.at(-1),backendClears:phase.seeks.reduce((sum,s)=>sum+s.backendClears,0),clearPackets:phase.seeks.reduce((sum,s)=>sum+s.clearPackets,0),stalePackets:phase.seeks.reduce((sum,s)=>sum+s.stalePackets,0),burstFinalDrawMs:phase.seeks.at(-1).drawMs};
  phase.ok=true;
  observing=false;await consumer.close();consumer=null;painter?.dispose();painter=null;worker?.dispose();worker=null;
  await wait(async()=>!(await reaper.video.getState()).stream,'stream shutdown');
 }
 result.ok=true;
}catch(e){result.error=String(e);result.stack=e.stack;result.state=await reaper.video.getDiagnostics().catch(()=>null);}
finally{await consumer?.close().catch(()=>{});painter?.dispose();worker?.dispose();}
await reaper.SetExtState('VideoDynamic','result',JSON.stringify(result),false);
</script>'''

RUNNER = r'''
local r=reaper
local root=r.GetResourcePath()
local function write(name,text)local f=assert(io.open(root..'/'..name,'w'));f:write(text);f:close()end
local function main()
 assert(r.APIExists('ReaWeb_Open'),'extension not loaded')
 r.GetSetProjectInfo(0,'PROJECT_SRATE',48000,true)
 local order={__ORDER__}
 for i,index in ipairs(order)do
  r.SelectAllMediaItems(0,false)
  r.SetEditCurPos((i-1)*__SECONDS__,false,false)
  r.InsertMedia(root..'/media/'..index..'.mp4',1)
  local item=assert(r.GetSelectedMediaItem(0,0),'media import failed')
  r.SetMediaItemInfo_Value(item,'D_POSITION',(i-1)*__SECONDS__)
  r.SetMediaItemInfo_Value(item,'D_LENGTH',__SECONDS__)
  r.SetMediaItemInfo_Value(item,'D_FADEINLEN',0)
  r.SetMediaItemInfo_Value(item,'D_FADEOUTLEN',0)
 end
 r.GetSetRepeat(0);r.SetEditCurPos(0.2,false,true);r.Audio_Init()
 r.Main_SaveProjectEx(0,root..'/fixture.rpp',0)
 local id=r.ReaWeb_Open(root..'/app/index.html');assert(id>0,r.ReaWeb_GetLastError())
 local begin=r.time_precise()
 local lastProgress=''
 local function poll()
  local progress=r.GetExtState('VideoDynamic','progress')
  if progress~=lastProgress then lastProgress=progress;write('progress.txt',progress)end
  local result=r.GetExtState('VideoDynamic','result')
  if result~=''then write('result.json',result);write('diagnostics.json',r.ReaWeb_GetDiagnostics(id));r.ReaWeb_Close(id);r.OnStopButton();return end
  if r.time_precise()-begin>__TIMEOUT__ then write('error.txt','Page timeout\n'..r.ReaWeb_GetDiagnostics(id));return end
  r.defer(poll)
 end
 r.defer(poll)
end
local ok,err=xpcall(main,debug.traceback);if not ok then write('error.txt',err)end
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True, help='New disposable profile directory')
    parser.add_argument('--reaper', type=Path, required=True)
    parser.add_argument('--extension', type=Path, required=True)
    parser.add_argument('--seconds', type=float, default=1.5, help='Duration of each timeline item')
    parser.add_argument('--modes', nargs='+', choices=MODES, default=list(MODES))
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--intra-only', action='store_true', help='Use all-keyframe media to separate decoder seek cost')
    args = parser.parse_args()
    if sys.platform != 'win32':
        parser.error('This acceptance harness requires Windows')
    if not 1 <= args.seconds <= 10:
        parser.error('--seconds must be between 1 and 10')
    root = args.profile.resolve()
    root.mkdir(parents=True, exist_ok=False)
    for name in ('UserPlugins', 'EmptyPlugins', 'Plugins', 'app', 'media'):
        (root / name).mkdir()
    source = args.reaper.resolve().parent
    shutil.copy2(args.reaper, root / 'reaper.exe')
    shutil.copy2(args.extension, root / 'UserPlugins' / args.extension.name)
    for name in ('reaper_video.dll', 'reaper_wave.dll', 'FFmpeg.dll'):
        shutil.copy2(source / 'Plugins' / name, root / 'Plugins' / name)
    for name in ('reaper-license.rk', 'reaper-install-rev.txt'):
        if (source / name).exists():
            shutil.copy2(source / name, root / name)
    config = ('[reaper]\nloadlastproj=0\nshowlastproj=0\nsplash=0\nverchk=0\nerrnowarn=5\n'
              'hasrecentlyopened=1\nnewprojdo=0\nvstfullstate=58181\nvstpath64=\nvstpath=\n'
              'projfrbase=30\nprojfrdrop=0\nclap_path_win64=' + str(root / 'EmptyPlugins') + '\n'
              '[audioconfig]\nmode=4\ndummy_srate=48000\ndummy_blocksize=256\n'
              'waveout_devicein=-1\nwaveout_deviceout=0\nwaveout_srate=48000\nwaveout_bps=16\n'
              'waveout_bs=256\nwaveout_numblocks=4\nwaveout_nch_out=2\n[reaper_video]\nvisible=0\n')
    (root / 'reaper.ini').write_text(config, encoding='utf-8')
    import imageio_ffmpeg
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    for index, (width, height) in enumerate(SIZES):
        subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
                        f'testsrc2=size={width}x{height}:rate=30', '-t', str(args.seconds),
                        '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '24', '-pix_fmt', 'yuv420p',
                        '-g', '1' if args.intra_only else '250',
                        '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709',
                        '-movflags', '+faststart', str(root / 'media' / f'{index}.mp4')], check=True)
    app_source = ROOT / 'examples/integrations/video-preview'
    for name in ('canvas-renderer.js', 'frame-renderer.js', 'i420-renderer.js', 'video-renderer.js',
                 'video-worker.js', 'worker-stream.js'):
        shutil.copy2(app_source / name, root / 'app' / name)
    (root / 'app/app.json').write_text(json.dumps({'id': 'video-dynamic-acceptance', 'entry': 'index.html',
                                                'videoHighRefreshRate': True}), encoding='utf-8')
    page = PAGE.replace('__MODES__', json.dumps(args.modes)).replace('__SIZES__', json.dumps(SIZES))
    page = page.replace('__ORDER__', json.dumps(ORDER)).replace('__SECONDS__', str(args.seconds))
    (root / 'app/index.html').write_text(page, encoding='utf-8')
    timeout = int(90 + (len(ORDER) * args.seconds + 20) * len(args.modes))
    runner = RUNNER.replace('__ORDER__', ','.join(map(str, ORDER))).replace('__SECONDS__', str(args.seconds))
    (root / 'runner.lua').write_text(runner.replace('__TIMEOUT__', str(timeout - 5)), encoding='utf-8')
    if args.prepare_only:
        print(root)
        return
    env = os.environ.copy()
    env['CLAP_PATH'] = str(root / 'EmptyPlugins')
    with (root / 'process.log').open('w') as log:
        process = subprocess.Popen([str(root / 'reaper.exe'), '-newinst', '-cfgfile', str(root / 'reaper.ini'),
                                    '-new', '-ignoreerrors', '-nosplash', str(root / 'runner.lua')],
                                   cwd=root, env=env, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if (root / 'result.json').exists() or (root / 'error.txt').exists() or process.poll() is not None:
                    break
                time.sleep(.5)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    if (root / 'error.txt').exists():
        raise RuntimeError((root / 'error.txt').read_text(encoding='utf-8', errors='replace'))
    if not (root / 'result.json').exists():
        raise RuntimeError(f'No acceptance result. Inspect {root}')
    result = json.loads((root / 'result.json').read_text(encoding='utf-8'))
    summary = {'ok': result['ok'], 'error': result.get('error'), 'phases': [
        {key: phase.get(key) for key in ('mode', 'ok', 'skipped', 'frames', 'drawn', 'dimensions', 'sourceSizes',
                                        'connectionsBefore', 'connectionsAfter', 'maxGapMs', 'seekSummary', 'latencyScope', 'errors', 'closed')}
        for phase in result['phases']]}
    print(json.dumps(summary, indent=2))
    raise SystemExit(0 if result['ok'] else 1)


if __name__ == '__main__':
    main()
