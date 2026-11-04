"""Exercise SDK-managed DSP streams in an explicitly supplied disposable REAPER profile."""
import argparse
import configparser
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
for name in ('reaper', 'extension', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
parser.add_argument('--diagnostics', action='store_true')
parser.add_argument('--realtime', action='store_true', help='Run a concurrent transport subscription during JSFX recovery')
args = parser.parse_args()
source = Path(__file__).resolve().parents[1]
root = args.output.resolve()
root.mkdir(parents=True, exist_ok=False)
(root / 'UserPlugins').mkdir()
binary = root / 'UserPlugins' / args.extension.name
shutil.copy2(args.extension, binary)
if sys.platform == 'darwin':
    subprocess.run(['codesign', '--force', '--sign', '-', str(binary)], check=True)
if sys.platform.startswith('linux'):
    helper = args.extension.parent / ('reawebapi-webview-' + args.extension.stem.split('reaper_reawebapi-', 1)[1])
    shutil.copy2(helper, root / 'UserPlugins' / helper.name)
    (root / 'UserPlugins' / helper.name).chmod(0o755)
effect = root / 'Effects/recovery'
effect.mkdir(parents=True)
shutil.copy2(source / 'runtime/jsfx/reaweb_jsfx.inc', effect)
shutil.copy2(source / 'runtime/jsfx-webui.mjs', effect)
code = (source / 'runtime/jsfx/examples/analyzer/analyzer.jsfx').read_text()
code = code.replace('reaweb_jsfx_publish(2,0,512,2,clock-256);',
                    'reaweb_jsfx_publish(2,0,512,2,clock-256);\n  reaweb_jsfx_publish(3,0,512,2,clock-256);')
# One complete analysis window per packet keeps acceptance independent of telemetry drops.
code = code.replace('collected==256', 'collected==2048').replace(',0,512,2,clock-256', ',0,4096,2,clock-2048')
code += '\n@serialize\nreaweb_jsfx_serialize(0,0,0);\nslider2=reaweb_jsfx_handshake(slider2);\n'
(effect / 'analyzer.jsfx').write_text(code)
config = json.loads((source / 'runtime/jsfx/examples/analyzer/analyzer.json').read_text())
config['JSFX_WebUI']['native']['streams'].append(dict(id='meter', kind='meter', source='input/test', channels=2))
(effect / 'analyzer.json').write_text(json.dumps(config))
(effect / 'index.html').write_text('''<!doctype html><meta charset="utf-8"><title>DSP stream recovery acceptance</title>
<p id="status">Connecting</p><script type="module">
import {connectJSFX} from './jsfx-webui.mjs';
const errors=[], counts={}, streams=[], names=['spectrum','waveform','scope','meter'];
const page=crypto.randomUUID();let busy=false, manual=false, lastCommand='';
const report=(e,origin='sdk')=>errors.push({code:e.code,message:e.message,origin,stack:e.stack});
const fx=await connectJSFX(reaper,report);
async function open(){
  for(let i=0;i<300&&(await fx.getCapabilities()).status!=='ready';i++)await new Promise(r=>setTimeout(r,100));
  await new Promise(r=>setTimeout(r,1000));
  for(const id of names){
    counts[id]=0;const stream=await fx.streams.open(id);streams.push(stream);
    stream.on('data',()=>{counts[id]++;if(id==='scope')while(stream.read()){};});
  }
}
const opening=open().catch(e=>report(e,'open'));
await fx.onVisibilityChange(active=>{if(active&&!manual&&streams.every(s=>s.closed)){streams.length=0;open().catch(e=>report(e,'reopen'));}});
setInterval(async()=>{
  if(busy)return;busy=true;
  try{
    const command=await reaper.GetExtState('DSPRecovery','pageCommand');
    if(command&&command!==lastCommand){lastCommand=command;if(command==='close'){manual=true;await Promise.all(streams.map(s=>s.close()));}}
    const diagnostics=await fx.getDiagnostics();
    const state={page,counts,errors,closed:streams.map(s=>s.closed),sessions:streams.map(s=>s.info.jsfx.sessionId),
      names:streams.map(s=>s.info.name),active:fx.active,diagnostics};
    document.getElementById('status').textContent=JSON.stringify(state);
    await reaper.SetExtState('DSPRecovery','snapshot',JSON.stringify(state),false);
  }catch(e){if(e.code!=='STALE_SESSION')report(e);}finally{busy=false;}
},100);
await opening;
</script>''')
(root / 'EmptyVST').mkdir()
if args.realtime:
    page = effect / 'index.html'
    text = page.read_text()
    text = text.replace("const opening=open().catch", "let realtimeCount=0; const realtime=await reaper.realtime.subscribe('transport',{updateRate:120,onData:()=>realtimeCount++,onError:report});\nconst opening=open().catch")
    text = text.replace('const state={page,counts,errors,', 'const state={realtimeCount,page,counts,errors,')
    page.write_text(text)
settings = ('[reaper]\nloadlastproj=0\nshowlastproj=0\nsplash=0\nverchk=0\nerrnowarn=5\nhasrecentlyopened=1\n'
            'newprojdo=0\nvst_scan=0\nvstfullstate=49989\nvstpath64=' + str(root / 'EmptyVST') + '\n')
if sys.platform == 'darwin':
    user = configparser.RawConfigParser(strict=False)
    user.read(Path.home() / 'Library/Application Support/REAPER/reaper.ini')
    settings += ''.join(f'{key}={value}\n' for key, value in user['reaper'].items() if key.startswith('coreaudio'))
elif sys.platform.startswith('linux'):
    settings += 'linux_audio_mode=3\nlinux_audio_srate=48000\nlinux_audio_bsize=512\nlinux_audio_nch_out=2\n'
else:
    settings += '[audioconfig]\nmode=4\nwaveout_devicein=-1\nwaveout_deviceout=0\nwaveout_srate=48000\nwaveout_bps=16\nwaveout_bs=256\nwaveout_numblocks=4\nwaveout_nch_out=2\n'
(root / 'reaper.ini').write_text(settings)
if args.diagnostics:
    (root / 'ReaWebAPI.ini').write_text('[Diagnostics]\nRuntimeDebug=1\nAppDebug=0\nTargetAppId=\n')
resource = Path.home() / 'Library/Application Support/REAPER' if sys.platform == 'darwin' else args.reaper.parent
for name in ('reaper-install-rev.txt', 'reaper-license.rk'):
    if (resource / name).is_file():
        shutil.copy2(resource / name, root / name)
lua = r'''
local root=ROOT
local function write(name,text) local f=assert(io.open(root..'/'..name,'w'));f:write(text);f:close() end
local function main()
write('started.txt','started')
reaper.InsertTrackAtIndex(0,false)
local track=reaper.GetTrack(0,0)
reaper.SetMediaTrackInfo_Value(track,'B_MAINSEND',0)
assert(reaper.TrackFX_AddByName(track,'recovery/analyzer.jsfx',false,-1)==0)
write('stage.txt','effect loaded')
reaper.TrackFX_SetParamNormalized(track,0,0,1)
reaper.TrackFX_Show(track,0,3)
write('stage.txt','editor opened')
reaper.OnPlayButton()
write('stage.txt','playing')
local last=''
local function tick()
  local ok,err=pcall(function()
    local snapshot=reaper.GetExtState('DSPRecovery','snapshot')
    if snapshot~='' then write('snapshot.json',snapshot) end
    local f=io.open(root..'/command.txt','r');local command=f and f:read('*a') or '';if f then f:close() end
    if command=='' or command==last then return end
    last=command
    local op=command:match(':(.*)')
    if op=='preset' then assert(reaper.TrackFX_SetPresetByIndex(track,0,-2));reaper.TrackFX_SetParamNormalized(track,0,0,1)
    elseif op=='recompile' then
      local file=assert(io.open(root..'/Effects/recovery/analyzer.jsfx','a'));file:write('\n// recovery acceptance recompilation\n');file:close()
      reaper.TrackFX_SetOffline(track,0,true);reaper.TrackFX_SetOffline(track,0,false)
    elseif op=='device-stop' then reaper.Audio_Quit()
    elseif op=='device-start' then reaper.Audio_Init()
    elseif op=='hide' then reaper.TrackFX_Show(track,0,2)
    elseif op=='show' then reaper.TrackFX_Show(track,0,3)
    elseif op=='close' then reaper.SetExtState('DSPRecovery','pageCommand','close',false)
    elseif op=='delete' then reaper.TrackFX_Delete(track,0)
    elseif op=='quit' then
      reaper.OnStopButton();reaper.Main_SaveProjectEx(0,root..'/acceptance.rpp',8);reaper.Main_SaveProject(0,false)
      reaper.Main_OnCommand(40004,0)
    else error('Unknown operation: '..op) end
    write('reply.txt',command)
  end)
  if not ok then write('error.txt',tostring(err)) end
  if not last:find(':quit',1,true) then reaper.defer(tick) end
end
reaper.defer(tick)
end
local ok,err=xpcall(main,debug.traceback)
if not ok then write('error.txt',err) end
'''
if args.realtime or (args.diagnostics and sys.platform == 'darwin'):
    lua = lua.replace('local ok,err=xpcall(main,debug.traceback)\nif not ok then write(\'error.txt\',err) end',
        "local ready=reaper.time_precise()+1\nlocal function start()\nif reaper.time_precise()<ready then reaper.defer(start);return end\nlocal ok,err=xpcall(main,debug.traceback)\nif not ok then write('error.txt',err) end\nend\nreaper.defer(start)")
(root / 'launch.lua').write_text(lua.replace('ROOT', '[[' + root.as_posix() + ']]'))
results = {}
with (root / 'process.log').open('w') as log:
    process = subprocess.Popen([str(args.reaper), '-newinst', '-cfgfile', str(root / 'reaper.ini'), '-new',
                                '-ignoreerrors', '-nosplash', str(root / 'launch.lua')], stdout=log, stderr=log)
    def wait(fn, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if (root / 'error.txt').exists():
                raise AssertionError((root / 'error.txt').read_text())
            try:
                value = fn()
                if value:
                    return value
            except (FileNotFoundError, json.JSONDecodeError):
                pass
            except OSError as error:
                if error.errno != 61:
                    raise
            if process.poll() is not None:
                raise RuntimeError('REAPER exited: ' + str(process.returncode))
            time.sleep(.1)
        raise AssertionError('Timed out waiting for DSP stream recovery')
    def snapshot():
        return wait(lambda: json.loads((root / 'snapshot.json').read_text()))
    def command(op):
        value = str(time.time_ns()) + ':' + op
        (root / 'command.tmp').write_text(value)
        (root / 'command.tmp').replace(root / 'command.txt')
        wait(lambda: (root / 'reply.txt').read_text() == value)
    def advancing(previous=None):
        def check():
            value = snapshot()
            assert not value['errors'], value['errors']
            if not value['active'] or len(value['closed']) != 4 or any(value['closed']):
                return None
            baseline = previous['counts'] if previous else {}
            if len(value['counts']) == 4 and all(n > baseline.get(k, 0) + 3 for k, n in value['counts'].items()):
                assert not any(value['closed']), value
                assert value['diagnostics']['native']['streams'] == 4, value
                return value
        return wait(check, 60)
    try:
        if args.realtime:
            wait(lambda: (root / 'started.txt').exists(), 90)
        results['initial'] = advancing()
        for op in ('preset', 'recompile'):
            before = snapshot(); command(op); time.sleep(1)
            results[op] = advancing(before)
            assert results[op]['page'] == results['initial']['page'], 'Page was replaced'
            print(op, 'PASS', flush=True)
        command('device-stop'); time.sleep(2); paused = snapshot(); time.sleep(2)
        assert snapshot()['counts'] == paused['counts'], 'Audio_Quit did not stop DSP'
        if args.realtime:
            assert snapshot()['realtimeCount'] > paused['realtimeCount'], 'Realtime stopped with the audio device'
        command('device-start'); results['device-restart'] = advancing(paused)
        assert results['device-restart']['page'] == results['initial']['page']
        print('device-restart PASS', flush=True)
        command('hide'); wait(lambda: all(snapshot()['closed']))
        command('show'); results['editor-reopen'] = advancing()
        command('close'); wait(lambda: all(snapshot()['closed']))
        command('preset'); time.sleep(2); results['manual-close'] = snapshot()
        assert all(results['manual-close']['closed'])
        assert results['manual-close']['diagnostics']['native']['streams'] == 0
        command('delete'); time.sleep(1)
        results['fx-delete'] = 'completed'
        if args.diagnostics:
            rows = [json.loads(line) for line in (root / 'ReaWebAPI/Diagnostics/runtime.log').read_text().splitlines()]
            events = {row['event'] for row in rows}
            for event in ('jsfx.instance.identified', 'jsfx.attach.end', 'jsfx.native.session.changed', 'jsfx.park', 'jsfx.binding.invalid'):
                assert event in events, (event, sorted(events))
            assert not any('compile' in event for event in events), events
            assert all('message' not in row and 'token' not in row for row in rows)
            assert list((root / 'ReaWebAPI/Diagnostics').glob('app-*.log'))
            results['persistent-diagnostics'] = sorted(events)
        (root / 'result.json').write_text(json.dumps(results, indent=2))
        print(json.dumps({'passed': True, 'scenarios': list(results), 'output': str(root)}), flush=True)
    finally:
        (root / 'observations.json').write_text(json.dumps(results, indent=2))
        try:
            command('quit'); process.wait(timeout=10)
        except Exception:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
