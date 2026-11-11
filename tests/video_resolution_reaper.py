"""Opt-in final-frame resolution and high-resolution transport acceptance."""
import struct
import sys
import shutil
from pathlib import Path
import zlib
import video_reaper as harness

SIZES = [(1280, 720), (1920, 1080), (2560, 1440), (3840, 2160),
         (4096, 2160), (720, 1280), (800, 600), (2560, 1080), (997, 563)]
original_png = harness.png


def fixtures(path, rgb):
    original_png(path, rgb)
    if path.name != 'red.png':
        return
    shutil.copy2(Path(__file__).resolve().parents[1] / 'examples/integrations/video-preview/frame-renderer.js', path.parent / 'app/frame-renderer.js')
    shutil.copy2(Path(__file__).resolve().parents[1] / 'examples/integrations/video-preview/canvas-renderer.js', path.parent / 'app/canvas-renderer.js')
    for width, height in SIZES:
        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
        edge = max(2, min(width, height) // 16)
        white = bytes([240, 240, 240])
        red = bytes([210, 30, 60])
        rows = b''.join(b'\0' + (white * width if y < edge or y >= height - edge else
                                white * edge + red * (width - 2 * edge) + white * edge) for y in range(height))
        (path.parent / f'frame-{width}-{height}.png').write_bytes(
            b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


harness.png = fixtures
harness.PAGE = r'''<!doctype html><meta charset="utf-8"><title>Resolution acceptance</title>
<canvas></canvas><script type="module">
import { createFrameRenderer } from "./frame-renderer.js";
import { createCanvasRenderer } from "./canvas-renderer.js";
(async()=>{
 const result={checks:[],outputs:[]};let stream,second,raf=0,pending,renderer,painter;
 const sleep=ms=>new Promise(r=>setTimeout(r,ms));
 const check=(value,label)=>{if(!value)throw Error(label);result.checks.push(label);};
 const wait=async fn=>{const until=performance.now()+10000;while(performance.now()<until){const value=await fn();if(value)return value;await sleep(40);}throw Error('Frame timeout');};
 const close=async()=>{renderer?.clear();renderer=null;await stream?.close();stream=null;await second?.close();second=null;pending=null;if(raf)cancelAnimationFrame(raf);raf=0;await sleep(150);};
 try{
  await reaper.lifecycle.ready;await reaper.window.show();await reaper.window.focus();
  const root=await reaper.GetResourcePath();result.audioDevice={};for(const key of ['MODE','SRATE','BSIZE'])result.audioDevice[key]=await reaper.GetAudioDeviceInfo(key);
  const select=async file=>{await close();await reaper.OnStopButton();while(await reaper.CountTracks(0))await reaper.DeleteTrack(await reaper.GetTrack(0,0));await reaper.SetEditCurPos(0,false,true);await reaper.InsertMedia(root+'/'+file,1);if(file.endsWith('.png'))await reaper.SetMediaItemInfo_Value(await reaper.GetMediaItem(0,0),'D_LENGTH',2);await sleep(100);};
  const info=()=>wait(async()=>{const i=await reaper.video.getSourceInfo();return i.available&&i;});
  const open=options=>wait(async()=>{try{return await reaper.video.openStream(options);}catch(e){if(e.code==='VIDEO_FRAME_PENDING')return false;throw e;}});
  const frame=()=>wait(async()=>{const f=stream.latest(),d=await reaper.video.getState();return d.status==='ready'&&f&&!f.discontinuity&&f;});
  const pixel=(f,x,y)=>Array.from(f.bytes.slice((y*stream.info.width+x)*4,(y*stream.info.width+x)*4+3)).join(',');
  if(!__PERF_ONLY__){
  const testCanvas=document.createElement('canvas');testCanvas.width=testCanvas.height=2;
  const testPainter=createCanvasRenderer(testCanvas), pixels=new Uint8Array([255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255]);
  testPainter.draw({bytes:pixels});
  const gl=testCanvas.getContext('webgl');
  if(gl){const actual=new Uint8Array(16);gl.readPixels(0,0,2,2,gl.RGBA,gl.UNSIGNED_BYTE,actual);check([...actual].join() === [...pixels.slice(8),...pixels.slice(0,8)].join(),'WebGL preserves RGBA pixels and top-down orientation');}
  testPainter.dispose();
  const fallbackCanvas=document.createElement('canvas');fallbackCanvas.width=fallbackCanvas.height=2;
  const context=fallbackCanvas.getContext('2d'), fallback=createCanvasRenderer({width:2,height:2,getContext:type=>type==='2d'?context:null});
  fallback.draw({bytes:pixels});check(fallback.kind==='canvas2d'&&[...context.getImageData(0,0,2,2).data].join()===[...pixels].join(),'Canvas 2D fallback preserves pixels');fallback.dispose();
  for(const [width,height] of __SIZES__){
   await select(`frame-${width}-${height}.png`);
   const source=await info();check(source.width===width&&source.height===height,`final dimensions ${width}x${height}`);
   stream=await open({resolution:'source'});const original=await frame();
   check(stream.info.width===width&&stream.info.height===height&&original.bytes.length===width*height*4,`original payload ${width}x${height}`);
   check(pixel(original,0,0)==='240,240,240'&&pixel(original,width>>1,height>>1)==='210,30,60',`original pixels ${width}x${height}`);
   result.outputs.push({source:[width,height],bytes:original.bytes.length,captureMs:(await reaper.video.getDiagnostics()).captureMs});
   await close();stream=await open({resolution:'fit',width:800,height:500});const fitted=await frame();
   const scale=Math.min(1,800/width,500/height);
   check(stream.info.width===Math.floor(width*scale)&&stream.info.height===Math.floor(height*scale),`fit dimensions ${width}x${height}`);
   check(pixel(fitted,0,0)==='240,240,240',`fit keeps source border ${width}x${height}`);
  }
  await close();await select('frame-720-1280.png');
  stream=await open({width:640,height:360});let f=await frame();
  check(pixel(f,0,180)==='0,0,0'&&pixel(f,320,180)==='210,30,60','custom preserves portrait aspect and black bars');
  second=await open({width:640,height:360});await wait(()=>second.latest());
  let conflict=false;try{await open({resolution:'source'});}catch(e){conflict=e.code==='VIDEO_PROFILE_CONFLICT';}
  check(conflict&&!second.closed&&!!second.latest(),'conflicting profile preserves other consumer');
  await close();stream=await open({width:640,height:360,preserveAspect:false});f=await frame();
  check(pixel(f,0,180)==='240,240,240','aspect toggle stretches source without baked black bars');
  await close();let limit=false;try{await open({width:8192,height:8192});}catch(e){limit=e.code==='VIDEO_RESOLUTION_LIMIT';}check(limit,'oversize output rejected before allocation');
  await select('frame-1280-720.png');stream=await open({resolution:'source'});await frame();
  await reaper.SetEditCurPos(2,false,false);await reaper.InsertMedia(root+'/frame-720-1280.png',0);await reaper.SetEditCurPos(2.5,false,true);
  await wait(()=>stream.closed);check((await reaper.video.getState()).error==='VIDEO_RESOLUTION_CHANGED','source dimension change closes stale profile');
  await close();const changed=await info();check(changed.width===720&&changed.height===1280,'new source dimensions can be resolved');
  }
  if(await reaper.GetExtState('VideoAcceptance','duration')){
   result.performance=[];
   for(const options of [{resolution:'fit',width:1280,height:720},{resolution:'source'}]){
    await select('motion.mp4');await info();stream=await open(options);await frame();
    const canvas=document.querySelector('canvas');canvas.width=stream.info.width;canvas.height=stream.info.height;canvas.style.width='960px';canvas.style.height='auto';painter ||= createCanvasRenderer(canvas);
    let received=0,drawn=0,last,sequenceGaps=0,active=false;
    renderer=createFrameRenderer(f=>{if(painter.draw(f)&&active)drawn++;});
    stream.on('data',f=>{renderer.push(f);if(active){received++;if(last!==undefined)sequenceGaps+=Number(f.sequence-last-1n);last=f.sequence;}});
    await reaper.OnPlayButton();await sleep(1000);const before=await reaper.video.getDiagnostics(),drops=renderer.dropped,start=performance.now();active=true;await sleep(6000);active=false;const seconds=(performance.now()-start)/1000,after=await reaper.video.getDiagnostics();
    result.performance.push({renderer:painter.kind,options,width:canvas.width,height:canvas.height,receivedFPS:received/seconds,canvasFPS:drawn/seconds,capturedFPS:(after.capturedFrames-before.capturedFrames)/seconds,captureMs:after.captureMs,sequenceGaps,rendererDrops:renderer.dropped-drops,clearedFrames:after.clearedFrames-before.clearedFrames});
   }
  }
  result.ok=true;
 }catch(e){result.ok=false;result.error=String(e);result.stack=e.stack;result.state=await reaper.video.getDiagnostics().catch(()=>null);}
 finally{await close();await reaper.OnStopButton();}
 await reaper.SetExtState('VideoAcceptance','result',JSON.stringify(result),false);
})();</script>'''.replace('__SIZES__', str(SIZES).replace('(', '[').replace(')', ']'))

if __name__ == '__main__':
    performance_only = '--performance-only' in sys.argv
    if performance_only:
        sys.argv.remove('--performance-only')
    harness.PAGE = harness.PAGE.replace('__PERF_ONLY__', str(performance_only).lower())
    launch = harness.subprocess.Popen
    def launch_profile(args, **kwargs):
        if '-cfgfile' not in args:
            return launch(args, **kwargs)
        config = Path(args[args.index('-cfgfile') + 1])
        text = config.read_text(encoding='utf-8')
        if sys.platform == 'win32':
            text = text.replace('mode=4\n', 'mode=4\ndummy_srate=48000\ndummy_blocksize=256\n')
        elif sys.platform.startswith('linux'):
            text = text.replace('linux_audio_mode=3', 'linux_audio_mode=2').replace('linux_audio_bsize=512', 'linux_audio_bsize=256')
        config.write_text(text, encoding='utf-8')
        return launch(args, **kwargs)
    harness.subprocess.Popen = launch_profile
    harness.main()
