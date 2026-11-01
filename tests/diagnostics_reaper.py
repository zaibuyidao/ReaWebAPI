"""Opt-in Diagnostics acceptance with copied JSFX assets. Keep the test desktop unlocked and REAPER foregrounded."""
from pathlib import Path
import argparse, configparser, json, shutil, subprocess, sys, time

PAGE_PROBE = r'''(async () => {
  await reaper.lifecycle.ready;
  await reaper.events.on('message', raw => {
    const op = JSON.parse(raw).op;
    if (op === 'freeze') setTimeout(() => { const end = performance.now() + 21000; while (performance.now() < end) {} }, 0);
    if (op === 'errors') {
      setTimeout(() => { throw new Error('diagnostic injected script error'); }, 0);
      Promise.reject(new Error('diagnostic injected rejection'));
      throw new Error('diagnostic injected callback error');
    }
  });
  await reaper.host.send('DIAGNOSTIC_READY');
})();'''

RUNNER = r'''
local r = reaper
local root = r.GetResourcePath():gsub('\\','/')
assert(root:find('diagnostics%-acceptance'), 'Disposable Diagnostics profile required')
local out = assert(io.open(root..'/snapshots.jsonl','w'))
local function status(s) local f=assert(io.open(root..'/status.txt','w'));f:write(s);f:close() end
local function wait(seconds) local deadline=r.time_precise()+seconds;repeat coroutine.yield() until r.time_precise()>=deadline end
local ids={}
local function snapshot(label)
 for _,id in ipairs(ids) do out:write('{"phase":"'..label..'","id":'..id..',"snapshot":'..r.ReaWeb_GetDiagnostics(id)..'}\n');out:flush() end
end
local function send(id,op) assert(r.ReaWeb_Send(id,'{"op":"'..op..'"}'),r.ReaWeb_GetLastError()) end
local co=coroutine.create(function()
 assert(r.APIExists('ReaWeb_Open'))
 status('initializing')
 r.Audio_Init();r.OnPlayButton()
 local app=r.ReaWeb_Open(root..'/Scripts/diagnostics-app/index.html');assert(app>0,r.ReaWeb_GetLastError());ids[1]=app
 local deadline=r.time_precise()+20
 while not r.ReaWeb_IsReady(app) and r.time_precise()<deadline do wait(.1) end
 assert(r.ReaWeb_IsReady(app),'App initialization timed out')
 local tracks={}
 for i,name in ipairs({'Motor/sbp_MotorSynth_v2.jsfx','Ultra/sbp_ultraComb_webui.jsfx'}) do
  r.InsertTrackAtIndex(i-1,false);local track=r.GetTrack(0,i-1);tracks[i]=track
  r.SetMediaTrackInfo_Value(track,'D_VOL',0)
  local fx=r.TrackFX_AddByName(track,'Diagnostics/'..name,false,-1);assert(fx>=0,'Cannot add '..name)
  r.TrackFX_Show(track,fx,3)
  local found=false;deadline=r.time_precise()+20
  repeat
   wait(.1)
   for id=app+1,app+10 do
    local known=false;for _,previous in ipairs(ids) do if previous==id then known=true end end
    if not known and r.ReaWeb_IsOpen(id) and r.ReaWeb_IsReady(id) then ids[#ids+1]=id;found=true;break end
   end
  until found or r.time_precise()>=deadline
  assert(found,'JSFX initialization timed out: '..name)
 end
 wait(12)
 assert(#ids==3,'Expected App plus two JSFX windows, got '..#ids)
 for _,id in ipairs(ids) do assert(r.ReaWeb_IsReady(id),'Bridge not ready '..id) end
 snapshot('baseline');status('errors')
 for _,id in ipairs(ids) do send(id,'errors') end
 wait(6);snapshot('errors')
 status('freeze')
 send(ids[1],'freeze');send(ids[3],'freeze')
 wait(19);snapshot('stalled');wait(8);snapshot('recovered')
 status('host-gap')
 local endtime=r.time_precise()+2.2;while r.time_precise()<endtime do end
 wait(1);snapshot('host-gap')
 status('park-reopen')
 for _,track in ipairs(tracks) do r.TrackFX_Show(track,0,2) end
 wait(3);snapshot('parked')
 for _,track in ipairs(tracks) do r.TrackFX_Show(track,0,3);wait(2) end
 wait(8);snapshot('reopened')
 r.OnStopButton()
 for _,track in ipairs(tracks) do r.DeleteTrack(track) end
 r.ReaWeb_Close(app);wait(2)
 snapshot('closed');out:close();status('complete')
 r.Main_OnCommand(40004,0)
end)
local function step()
 local ok,err=coroutine.resume(co)
 if not ok then status('ERROR '..tostring(err));out:close();return end
 if coroutine.status(co)~='dead' then r.defer(step) end
end
step()
'''

def prepare(args):
    profile = args.profile.resolve()
    if 'diagnostics-acceptance' not in profile.name:
        raise ValueError('Use a new profile whose name contains diagnostics-acceptance')
    if profile.exists():
        raise FileExistsError(profile)
    (profile/'UserPlugins').mkdir(parents=True)
    (profile/'Scripts/diagnostics-app').mkdir(parents=True)
    shutil.copy2(args.extension, profile/'UserPlugins'/args.extension.name)
    if sys.platform == 'win32':
        source = args.reaper.parent
        shutil.copy2(args.reaper, profile/'reaper.exe')
        (profile/'Plugins').mkdir()
        for name in ['jsfx.dll','reaper_midi.dll','reaper_wave.dll']:
            shutil.copy2(source/'Plugins'/name, profile/'Plugins'/name)
        if (source/'reaper-license.rk').exists(): shutil.copy2(source/'reaper-license.rk',profile)
    else:
        license = Path.home()/'Library/Application Support/REAPER/reaper-license.rk'
        if license.exists(): shutil.copy2(license,profile)
        subprocess.run(['codesign','--force','--sign','-',str(profile/'UserPlugins'/args.extension.name)],check=True)
    for source,name,entry in [(args.motor,'Motor','index.html'),(args.ultra,'Ultra','packed.html')]:
        dest=profile/'Effects/Diagnostics'/name
        shutil.copytree(source,dest)
        p=dest/'webui'/entry
        html=p.read_text(encoding='utf-8')
        assert '<head>' in html
        p.write_text(html.replace('<head>','<head><script defer src="diagnostic-test.js"></script>',1),encoding='utf-8')
        (p.parent/'diagnostic-test.js').write_text(PAGE_PROBE,encoding='utf-8')
    app=profile/'Scripts/diagnostics-app'
    (app/'app.json').write_text('{"id":"diagnostics-acceptance","name":"Diagnostics acceptance"}')
    (app/'index.html').write_text('<!doctype html><title>Diagnostics acceptance</title><script defer src="diagnostic-test.js"></script><h1>Diagnostics acceptance</h1>')
    (app/'diagnostic-test.js').write_text(PAGE_PROBE,encoding='utf-8')
    (profile/'runner.lua').write_text(RUNNER,encoding='utf-8')
    (profile/'ReaWebAPI.ini').write_text(f'[Diagnostics]\nRuntimeDebug={int(args.monitoring == "on")}\nAppDebug=0\n')
    (profile/'EmptyPlugins').mkdir()
    (profile/'reaper.ini').write_text('[reaper]\nmixwnd_vis=0\nverchk=0\nerrnowarn=5\nloadlastproj=0\nshowlastproj=0\nsplash=0\nhasrecentlyopened=1\nnewprojdo=0\nvst_scan=0\nvstfullstate=49989\nvstpath64='+str(profile/'EmptyPlugins')+'\nvstpath='+str(profile/'EmptyPlugins')+'\nlv2path_win64=;\nclap_path_win64=\n[audioconfig]\nmode=4\n')
    if sys.platform == 'win32':
        ini = profile/'reaper.ini'
        ini.write_text(ini.read_text().replace('[audioconfig]', 'audiocloseinactive=0\naudioclosestop=0\n[audioconfig]'))
    if sys.platform == 'darwin':
        settings = configparser.RawConfigParser(strict=False)
        settings.read(Path.home()/'Library/Application Support/REAPER/reaper.ini')
        if settings.has_section('reaper'):
            audio = ''.join(f'{key}={value}\n' for key,value in settings['reaper'].items() if key.startswith('coreaudio'))
            ini = profile/'reaper.ini'
            ini.write_text(ini.read_text().replace('[audioconfig]',audio+'[audioconfig]'))

def run(args):
    profile=args.profile.resolve()
    if sys.platform == 'darwin' and '"CGSSessionScreenIsLocked"=Yes' in subprocess.check_output(['ioreg','-n','Root','-d1'],text=True):
        raise RuntimeError('Unlock the macOS desktop before REAPER acceptance')
    executable=profile/'reaper.exe' if sys.platform=='win32' else args.reaper
    command=[str(executable),'-newinst','-cfgfile',str(profile/'reaper.ini'),'-nosplash',str(profile/'runner.lua')]
    with (profile/'process.log').open('w') as log:
        process=subprocess.Popen(command,cwd=profile,stdout=log,stderr=log)
        previous=''
        try:
            deadline=time.monotonic()+150
            while time.monotonic()<deadline:
                status=(profile/'status.txt').read_text(encoding='utf-8') if (profile/'status.txt').exists() else 'launching'
                if status!=previous: print(status,flush=True);previous=status
                if status=='complete':break
                if status.startswith('ERROR'):raise RuntimeError(status)
                if process.poll() is not None:raise RuntimeError(f'REAPER exited {process.returncode}: {status}')
                time.sleep(.5)
            else:raise TimeoutError(previous)
        finally:
            if process.poll() is None:
                try:process.wait(timeout=8)
                except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=10)
    rows=[json.loads(line) for line in (profile/'snapshots.jsonl').read_text(encoding='utf-8').splitlines()]
    events=[json.loads(line) for line in (profile/'ReaWebAPI/Diagnostics/runtime.log').read_text(encoding='utf-8').splitlines()]
    baseline=[row for row in rows if row['phase']=='baseline']
    assert len(baseline)==3
    for row in baseline:
        snapshot=row['snapshot']
        assert snapshot['version']=='0.3.9.18',snapshot['version']
        assert snapshot['bridgeHealth']['enabled'] == (args.monitoring == 'on'),snapshot
        if args.monitoring == 'on': assert snapshot['bridgeHealth'].get('pagePending') is not None,snapshot
        if 'jsfx' in snapshot:
            assert snapshot['jsfx']['presented'],snapshot
            if args.monitoring == 'on': assert snapshot['bridgeHealth'].get('streamPackets',0)>0,snapshot
    if args.monitoring == 'on':
        for row in [r for r in rows if r['phase']=='errors']:
            health=row['snapshot']['bridgeHealth']
            assert all(health.get(k,0)>=1 for k in ['scriptErrors','promiseErrors','callbackErrors']),health
        for event in ['bridge.unresponsive','bridge.responsive','runtime.tick.delayed']:
            assert any(row['event']==event for row in events),event
        for event in ['bridge.unresponsive','bridge.responsive']:
            assert {row['windowId'] for row in events if row['event']==event} == {baseline[0]['id'],baseline[2]['id']},event
    else:
        active_events = {'bridge.health','bridge.unresponsive','bridge.responsive','bridge.slow','bridge.probe.fail',
                         'bridge.request.stalled','javascript.health.warning','runtime.tick.delayed','api.slow'}
        assert not any(row['event'] in active_events for row in events)
        for row in rows:
            health = row['snapshot']['bridgeHealth']
            assert health == {'enabled':False,'probePending':False,'unresponsive':False},health
        app_events = [json.loads(line) for path in (profile/'ReaWebAPI/Diagnostics').glob('app-*.log')
                      for line in path.read_text(encoding='utf-8').splitlines()]
        assert any(row['event']=='javascript.log' and row['level']=='ERROR' for row in app_events)
    for row in [r for r in rows if r['phase']=='reopened' and 'jsfx' in r['snapshot']]:
        assert row['snapshot']['jsfx']['presented'] and not row['snapshot']['jsfx']['cached'],row
    for row in [r for r in rows if r['phase']=='closed']:assert row['snapshot']['stage']=='closed',row
    counts={name:sum(row['event']==name for row in events) for name in sorted({row['event'] for row in events})}
    (profile/'result.json').write_text(json.dumps({'passed':True,'monitoring':args.monitoring,'events':counts},indent=2))
    print(json.dumps(counts,indent=2),flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--profile',type=Path,required=True)
    parser.add_argument('--reaper',type=Path,required=True)
    parser.add_argument('--extension',type=Path)
    parser.add_argument('--motor',type=Path)
    parser.add_argument('--ultra',type=Path)
    parser.add_argument('--prepare',action='store_true')
    parser.add_argument('--run',action='store_true')
    parser.add_argument('--monitoring',choices=['on','off'],default='on')
    args=parser.parse_args()
    if args.prepare:prepare(args)
    if args.run:run(args)
