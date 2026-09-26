"""Exercise native communication in an isolated real REAPER process."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reaper', type=Path, required=True)
parser.add_argument('--extension', type=Path, required=True)
parser.add_argument('--service-extension', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--coexist', action='store_true')
args = parser.parse_args()
root = args.output.resolve()
root.mkdir(parents=True, exist_ok=False)
(root / 'UserPlugins').mkdir()
config = '[REAPER]\nloadlastproj=0\nsplash=0\nverchk=0\n'
if sys.platform.startswith('linux'):
    config += 'linux_audio_mode=3\nlinux_audio_srate=48000\nlinux_audio_bsize=512\n'
if sys.platform == 'darwin':
    config += 'hasrecentlyopened=1\n[audioconfig]\nmode=4\n'
(root / 'reaper.ini').write_text(config, encoding='utf-8')
for binary in (args.extension, args.service_extension):
    target = root / 'UserPlugins' / binary.name
    shutil.copy2(binary, target)
    if sys.platform == 'darwin':
        subprocess.run(['codesign', '--force', '--sign', '-', str(target)], check=True)
if sys.platform.startswith('linux'):
    for helper in args.extension.parent.glob('reawebapi-webview-*'):
        shutil.copy2(helper, root / 'UserPlugins' / helper.name)
(root / 'index.html').write_text('<!doctype html><meta charset="utf-8"><title>Native communication check</title><script src="app.js" defer></script>', encoding='utf-8')
script = r'''
(async () => {
  const rows = [], states = new Map(), pause = ms => new Promise(resolve => setTimeout(resolve, ms));
  const check = (ok, label) => { if (!ok) throw Error(label); rows.push(label); };
  const until = async (test, label) => {
    for (let i=0; i<250; ++i) { if (test()) { rows.push(label); return; } await pause(20); }
    throw Error('Timed out: '+label);
  };
  const failure = async (promise, code) => {
    try { await promise; } catch (error) { check(error.code === code, code); return; }
    throw Error('Expected '+code);
  };
  try {
    await reaper.lifecycle.ready;
    check(await reaper.fs.readText('launcher-returned.txt') === 'returned', 'Lua launcher returned');
    check((await reaper.host.service('runtime').invoke('getInfo')).version === '0.3.6.4', 'built-in service');
    const disposers=[];
    for (const name of ['trackSelectionChanged','trackStateChanged','transportChanged','projectChanged',
      'markersChanged','regionsChanged','currentRegionChanged','loopPointsChanged','timeSelectionChanged'])
      disposers.push(await reaper.events.on(name, data => states.set(name,data)));
    await until(() => states.size===9,'initial native snapshots');
    check([...states.values()].every(value=>value.available), 'all state APIs available');
    const tracks=[]; for(let i=0;i<3;++i) tracks.push(await reaper.GetTrack(0,i));
    await reaper.SetOnlyTrackSelected(tracks[0]);
    await until(()=>states.get('trackSelectionChanged').count===1,'single selection');
    await reaper.SetTrackSelected(tracks[1],true);
    await until(()=>states.get('trackSelectionChanged').count===2,'multiple selection');
    const selectionRevision=states.get('trackSelectionChanged').revision;
    await reaper.SetTrackSelected(tracks[1],false); await reaper.SetTrackSelected(tracks[2],true);
    await until(()=>states.get('trackSelectionChanged').revision>selectionRevision && states.get('trackSelectionChanged').count===2,'same-first selection replacement');
    for(const track of tracks) await reaper.SetTrackSelected(track,false);
    await until(()=>states.get('trackSelectionChanged').count===0,'empty selection');
    const trackRevision=states.get('trackStateChanged').revision;
    await reaper.GetSetMediaTrackInfo_String(tracks[0],'P_NAME','Native state test',true);
    await reaper.SetMediaTrackInfo_Value(tracks[0],'B_MUTE',1);
    await until(()=>states.get('trackStateChanged').revision>trackRevision,'track metadata');
    await reaper.CSurf_OnPlayRateChange(0.75);
    await until(()=>states.get('transportChanged').rate===0.75,'playback rate');
    await reaper.SetEditCurPos(2,false,false);
    await until(()=>states.get('transportChanged').cursor===2,'edit cursor');
    await reaper.OnPlayButton();
    await until(()=>states.get('transportChanged').playing,'play');
    await reaper.OnPauseButton();
    await until(()=>states.get('transportChanged').paused,'pause');
    await reaper.OnStopButton();
    await until(()=>states.get('transportChanged').state===0,'stop');
    await reaper.GetSetRepeat(1);
    await until(()=>states.get('transportChanged').loop,'loop state');
    await reaper.GetSet_LoopTimeRange2(0,true,true,1,4,false);
    await until(()=>states.get('loopPointsChanged').end===4,'loop points');
    await reaper.GetSet_LoopTimeRange2(0,true,false,2,5,false);
    await until(()=>states.get('timeSelectionChanged').end===5,'time selection');
    const marker=await reaper.AddProjectMarker2(0,false,1,0,'native marker',-1,0);
    const region=await reaper.AddProjectMarker2(0,true,1,4,'native region',-1,0);
    await until(()=>states.get('markersChanged').count===1 && states.get('regionsChanged').count===1,'marker and region additions');
    await until(()=>states.get('currentRegionChanged').region?.name==='native region','current region');
    const markerRevision=states.get('markersChanged').revision;
    await reaper.SetProjectMarker3(0,marker,false,1,0,'renamed marker',0);
    await until(()=>states.get('markersChanged').revision>markerRevision,'marker metadata');
    await reaper.SetProjectMarker3(0,region,true,1,4,'renamed region',0);
    await until(()=>states.get('currentRegionChanged').region?.name==='renamed region','region metadata');
    await reaper.Main_SaveProjectEx(0,PROJECT_PATH,8);
    await until(()=>states.get('projectChanged').projects.some(project=>project.path.endsWith('native.rpp')&&!project.dirty),'project path and clean state');
    await reaper.MarkProjectDirty(0);
    await until(()=>states.get('projectChanged').projects.some(project=>project.dirty),'project dirty state');
    const original=states.get('projectChanged').activeProject;
    await reaper.Main_OnCommand(40859,0);
    await until(()=>states.get('projectChanged').projects.length===2 && states.get('projectChanged').activeProject!==original,'project opened and switched');
    await reaper.Main_OnCommand(40860,0);
    await until(()=>states.get('projectChanged').projects.length===1 && states.get('projectChanged').activeProject===original,'project closed and switched');
    const service=reaper.host.service('test');
    check(await service.invoke('ping')==='pong','third-party invoke');
    let received=0, unloaded=false;
    const callback=()=>++received;
    const stop=await service.on('changed',callback);
    const stopUnload=await service.on('unloaded',()=>{unloaded=true;});
    service.send('message',{hello:1}); await until(()=>received===1,'third-party send and event');
    await service.off('changed',callback); await stop(); await stop();
    service.send('message',{}); await pause(150); check(received===1,'off and repeat dispose');
    await failure(service.invoke('missing'),'METHOD_NOT_FOUND');
    await failure(reaper.host.service('missing').invoke('ping'),'SERVICE_NOT_FOUND');
    if (COEXIST) {
      let echo=''; const stopEcho=await reaper.events.on('message',text=>{echo=text;});
      await reaper.host.send({method:'echo',payload:'native coexist'});
      await until(()=>echo==='{"method":"echo","payload":"native coexist"}','Lua defer coexistence');
      await stopEcho();
    }
    const pending=failure(service.invoke('pending'),'EXTENSION_UNLOADED');
    service.send('unregister'); await pending;
    await until(()=>unloaded,'unload notification'); await stopUnload();
    await failure(service.invoke('ping'),'SERVICE_NOT_FOUND');
    await reaper.window.setDocked(true); check(await reaper.window.isDocked(),'dock');
    await reaper.window.setDocked(false); check(!await reaper.window.isDocked(),'undock');
    for(const dispose of disposers) await dispose();
    await reaper.fs.writeText('result.json',JSON.stringify({passed:true,rows}),{overwrite:true});
    await reaper.window.close();
  } catch(error) {
    await reaper.fs.writeText('result.json',JSON.stringify({passed:false,error:String(error),code:error.code,rows,states:Object.fromEntries(states)}),{overwrite:true});
  }
})();
'''.replace('PROJECT_PATH', json.dumps(str(root / 'native.rpp'))).replace('COEXIST', 'true' if args.coexist else 'false')
(root / 'app.js').write_text(script, encoding='utf-8')
launcher = '''local root = debug.getinfo(1, 'S').source:sub(2):match('^(.*[/\\\\])')
assert(reaper.GetResourcePath():gsub('[/\\\\]+$', '') == root:gsub('[/\\\\]+$', ''))
for i=0,2 do reaper.InsertTrackAtIndex(i, true) end
local id = reaper.ReaWeb_Open(root .. 'index.html')
assert(id > 0, reaper.ReaWeb_GetLastError())
local file = assert(io.open(root .. 'launcher-returned.txt','w'))
file:write('returned') file:close()
'''
if args.coexist:
    launcher += '''local function loop()
  if not reaper.ReaWeb_IsOpen(id) then return end
  for _=1,32 do
    local value=reaper.ReaWeb_Receive(id)
    if value=='' then break end
    reaper.ReaWeb_Send(id,value)
  end
  reaper.defer(loop)
end
reaper.defer(loop)
'''
(root / 'launch.lua').write_text(launcher, encoding='utf-8')
command = [str(args.reaper.resolve()), '-newinst', '-cfgfile', str(root / 'reaper.ini'), '-new', '-nosplash', str(root / 'launch.lua')]
report = root / 'result.json'
with (root / 'process.log').open('w', encoding='utf-8') as log:
    process = subprocess.Popen(command, stdout=log, stderr=log)
    try:
        deadline = time.monotonic() + 90
        while not report.exists() and process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.2)
        result = json.loads(report.read_text(encoding='utf-8')) if report.exists() else {'passed':False,'error':'No report'}
        print(json.dumps({'resource':str(root), **result}, ensure_ascii=False), flush=True)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
raise SystemExit(0 if result['passed'] else 1)
