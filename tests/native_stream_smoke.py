"""Validate native streams and platform additions in an isolated real REAPER."""
import argparse
import configparser
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import wave

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reaper', type=Path, required=True)
parser.add_argument('--extension', type=Path, required=True)
parser.add_argument('--producer', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--audio-config', type=Path)
args = parser.parse_args()
root = args.output.resolve()
root.mkdir(parents=True, exist_ok=False)
(root / 'UserPlugins').mkdir()
config = '[REAPER]\nerrnowarn=5\nloadlastproj=0\nshowlastproj=0\nsplash=0\nverchk=0\naudioclosestop=0\naudiocloseinactive=0\n'
if sys.platform == 'darwin':
    config += 'hasrecentlyopened=1\n[audioconfig]\nmode=4\n'
elif sys.platform == 'win32':
    config += '[audioconfig]\nmode=4\nwaveout_devicein=-1\nwaveout_deviceout=0\nwaveout_srate=48000\nwaveout_bps=16\nwaveout_bs=256\nwaveout_numblocks=4\nwaveout_nch_in=0\nwaveout_nch_out=2\n'
if sys.platform.startswith('linux'):
    config += 'linux_audio_mode=3\nlinux_audio_srate=48000\nlinux_audio_bsize=512\nlinux_audio_nch_out=2\n'
if args.audio_config:
    source = configparser.ConfigParser(strict=False, interpolation=None)
    source.read(args.audio_config, encoding='utf-8-sig')
    config = config.split('[audioconfig]')[0] + '[audioconfig]\n' + ''.join(f'{key}={value}\n' for key, value in source['audioconfig'].items())
(root / 'reaper.ini').write_text(config, encoding='utf-8')
for binary in (args.extension, args.producer):
    target = root / 'UserPlugins' / binary.name
    shutil.copy2(binary, target)
    if sys.platform == 'darwin':
        subprocess.run(['codesign', '--force', '--sign', '-', str(target)], check=True)
if sys.platform.startswith('linux'):
    for helper in args.extension.parent.glob('reawebapi-webview-*'):
        shutil.copy2(helper, root / 'UserPlugins' / helper.name)
with wave.open(str(root / 'tone.wav'), 'wb') as wav:
    wav.setparams((2, 2, 48000, 0, 'NONE', ''))
    wav.writeframes(b''.join(struct.pack('<hh', *([int(8192 * math.sin(2 * math.pi * 440 * i / 48000))] * 2)) for i in range(48000 * 20)))
(root / 'index.html').write_text('<!doctype html><title>Native Stream verification</title><canvas width="240" height="160"></canvas><script src="app.js"></script>', encoding='utf-8')
(root / 'app.js').write_text(r'''
(async()=>{
 const checks=[],metrics={},sleep=ms=>new Promise(r=>setTimeout(r,ms));
 const check=(value,name)=>{if(!value)throw Error(name);checks.push(name);};
 const until=async(test,name)=>{for(let i=0;i<300;i++){if(test()){checks.push(name);return;}await sleep(20);}throw Error('Timeout: '+name);};
 try{
  await reaper.lifecycle.ready;
  check(await reaper.fs.readText('launcher-returned.txt')==='returned','Lua launcher returned');
  await reaper.window.focus();const service=reaper.host.service('stream.test');
  await service.invoke('start');
  const stream=await reaper.stream.open('test.video'),other=await reaper.stream.open('test.video');
  const canvas=document.querySelector('canvas'),gl=canvas.getContext('webgl2',{preserveDrawingBuffer:true});check(gl,'WebGL2');
  const shader=(kind,source)=>{const s=gl.createShader(kind);gl.shaderSource(s,source);gl.compileShader(s);check(gl.getShaderParameter(s,gl.COMPILE_STATUS),'shader compile');return s;};
  const program=gl.createProgram();
  gl.attachShader(program,shader(gl.VERTEX_SHADER,'#version 300 es\nconst vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));out vec2 uv;void main(){gl_Position=vec4(p[gl_VertexID],0,1);uv=(p[gl_VertexID]+1.0)/2.0;}'));
  gl.attachShader(program,shader(gl.FRAGMENT_SHADER,'#version 300 es\nprecision mediump float;in vec2 uv;uniform sampler2D tex;out vec4 color;void main(){color=texture(tex,uv);}'));
  gl.linkProgram(program);check(gl.getProgramParameter(program,gl.LINK_STATUS),'WebGL link');gl.useProgram(program);
  gl.bindTexture(gl.TEXTURE_2D,gl.createTexture());gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);
  let last=0n,drawn=0,render=true;
  const draw=()=>{if(!render)return;const packet=stream.latest();if(packet&&packet.sequence!==last){last=packet.sequence;drawn++;gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,240,160,0,gl.RGBA,gl.UNSIGNED_BYTE,packet.bytes);gl.drawArrays(gl.TRIANGLES,0,3);}requestAnimationFrame(draw);};requestAnimationFrame(draw);
  await until(()=>{metrics.deliveredSequence=String(stream.latest()?.sequence);metrics.drawn=drawn;metrics.hidden=document.hidden;return drawn>40;},'native frames reach rAF and WebGL');
  const pixel=new Uint8Array(4);gl.readPixels(1,1,1,1,gl.RGBA,gl.UNSIGNED_BYTE,pixel);check(pixel[1]===64&&pixel[2]===128&&pixel[3]===255,'binary pixel integrity');
  const before=await service.invoke('stats'),at=performance.now(),startDraw=drawn;await sleep(2000);
  const after=await service.invoke('stats');metrics.producerFps=(after.frames-before.frames)*1000/(performance.now()-at);metrics.webglFps=(drawn-startDraw)/2;
  check(metrics.producerFps>50&&metrics.producerFps<70,'independent ~60 Hz producer');
  const stalledAt=stream.latest().sequence,end=performance.now()+800;while(performance.now()<end){}
  await sleep(150);check(stream.latest().sequence>stalledAt+35n,'consumer stall skips obsolete frames');
  metrics.maxPublishUs=(await service.invoke('stats')).maxPublishUs;
  await other.close();await sleep(50);check(!stream.closed&&stream.latest(),'one consumer detach preserves another');
  await service.invoke('stop');await until(()=>stream.closed,'producer close notification');render=false;
  await service.invoke('start');const reopened=await reaper.stream.open('test.video');await until(()=>reopened.latest(),'stream reopen');await reopened.close();await service.invoke('stop');
  await reaper.SetEditCurPos(0,false,false);await reaper.OnPlayButton();await sleep(300);metrics.audioDevice=(await reaper.system.getDevices()).audio;
  for(const kind of ['audio','spectrum','meter','waveform']){
   const source=await reaper.audio.openStream(kind,{source:'master',fftSize:2048,updateRate:30});
   await until(()=>source.latest(),'hardware '+kind+' binary delivery');
   check(source.latest().data instanceof Float32Array,kind+' Float32 payload');
   if(kind==='audio'){await sleep(600);check(source.dropped>0,'audio consumer FIFO overrun');while(source.read()){}check(source.read()===null,'audio FIFO underrun');}
   else if(kind==='spectrum'){const bins=source.latest().data;check(bins.length===2050,'FFT bins');}
   else if(kind==='meter'){await sleep(500);check(source.latest().data.length===8,'Peak RMS LUFS layout');check(source.latest().data[0]>0,'master meter signal');}
   else check(source.latest().data.length===1024,'realtime min/max waveform');
   await source.close();await sleep(60);
  }
  const selected=await reaper.audio.openStream('spectrum',{source:'selected-track'});await until(()=>selected.latest(),'selected-track native analysis');await selected.close();
  const info=await reaper.audio.getWaveform('tone.wav',{points:128,start:1,duration:2});check(info.points>0&&info.data.length===2,'cached zoom-dependent waveform peaks');
  await reaper.OnStopButton();
  const devices=await reaper.system.getDevices();check(Array.isArray(devices.midiInputs)&&Array.isArray(devices.audio.outputs),'device inventory');
  const midi=await reaper.system.openMIDIInput();check(midi.info.kind==='midi','native MIDI consumer attach');
  const midiEvents=[];midi.on('data',packet=>midiEvents.push(Array.from(packet.bytes.subarray(16))));await sleep(100);
  await reaper.StuffMIDIMessage(0,0x90,60,100);await reaper.StuffMIDIMessage(0,0xb0,1,64);await reaper.StuffMIDIMessage(0,0x80,60,0);
  await until(()=>[0x90,0xb0,0x80].every(status=>midiEvents.some(data=>data[0]===status)),'real REAPER MIDI note on CC note off');await midi.close();
  const displays=await reaper.system.getDisplays();check(displays.length>0&&displays.every(d=>d.dpi>0&&d.bounds.width>0),'monitor geometry and DPI');
  await reaper.clipboard.writeBinary('application/x-reaweb-test',new Uint8Array([0,255,128,1]));
  check(String(await reaper.clipboard.readBinary('application/x-reaweb-test'))==='0,255,128,1','binary clipboard roundtrip');
  await reaper.clipboard.writeText('Native Stream test');check(await reaper.clipboard.readText()==='Native Stream test','text clipboard compatibility');
  let fired=0;await reaper.system.schedule(()=>fired++,{delay:20});await until(()=>fired===1,'native one-shot timer');await sleep(100);check(fired===1,'one-shot disposal');
  const stopTimer=await reaper.system.schedule(()=>fired++,{delay:10,interval:20});await until(()=>fired>=4,'native repeating timer');await stopTimer();
  await reaper.fs.makeDirectory('watched');const changes=[];const unwatch=await reaper.fs.watch('watched',e=>changes.push(e));
  await reaper.fs.writeText('watched/a.txt','first');await until(()=>changes.some(e=>e.type==='created'),'native file created');
  await reaper.fs.writeText('watched/a.txt','modified',{overwrite:true});await until(()=>changes.some(e=>e.type==='changed'),'native file changed');await unwatch();
  const diagnostic=await reaper.debug.getDiagnostics();check(diagnostic.system.processCpuSeconds>=0,'CPU diagnostics');
  await reaper.fs.writeText('result.json',JSON.stringify({passed:true,checks,metrics}));
 }catch(error){metrics.diagnostics=await reaper.debug.getDiagnostics();await reaper.fs.writeText('result.json',JSON.stringify({passed:false,checks,metrics,error:String(error),stack:error.stack}));}
})();
''', encoding='utf-8')
(root / 'launch.lua').write_text(r'''
local root=debug.getinfo(1,'S').source:sub(2):match('^(.*[/\\])')
local stage=assert(io.open(root..'launcher-started.txt','w'));stage:write('started');stage:close()
local ok,err=xpcall(function()
reaper.InsertMedia(root..'tone.wav',0)
local id=reaper.ReaWeb_Open(root..'index.html')
assert(id>0,reaper.ReaWeb_GetLastError())
local f=assert(io.open(root..'launcher-returned.txt','w'));f:write('returned');f:close()
end,debug.traceback)
if not ok then local f=io.open(root..'launcher-error.txt','w');f:write(err);f:close() end
''', encoding='utf-8')
command = [str(args.reaper.resolve()), '-newinst', '-cfgfile', str(root / 'reaper.ini'), '-new', '-nosplash', str(root / 'launch.lua')]
with (root / 'process.log').open('w', encoding='utf-8') as log:
    process = subprocess.Popen(command, stdout=log, stderr=log, startupinfo=None)
    try:
        deadline = time.monotonic() + 85
        while not (root / 'result.json').exists() and process.poll() is None and time.monotonic() < deadline:
            time.sleep(.2)
        result = json.loads((root / 'result.json').read_text(encoding='utf-8')) if (root / 'result.json').exists() else {'passed': False, 'error': 'No report'}
        print(json.dumps({'resource': str(root), **result}, ensure_ascii=False), flush=True)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
raise SystemExit(0 if result['passed'] else 1)
