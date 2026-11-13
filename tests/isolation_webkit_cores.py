"""Run the Windows core behavior fixtures in isolated macOS/Linux REAPER profiles."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from isolation_fixtures import dos_program, nes_program

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reaper', type=Path, required=True)
parser.add_argument('--extension', type=Path, required=True)
parser.add_argument('--emulator', type=Path, required=True)
parser.add_argument('--keyboard-probe', type=Path)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--core', choices=['dosbox_pure', 'fceumm', 'ppsspp'], required=True)
parser.add_argument('--psp-program', type=Path)
parser.add_argument('--psp-scenario', choices=['cube', 'controller', 'audio'], default='cube')
parser.add_argument('--single-thread', action='store_true')
parser.add_argument('--soak-seconds', type=int, default=0)
parser.add_argument('--reopen', type=int, default=0)
parser.add_argument('--memory-diagnostics', action='store_true', help='Sample private/PSS memory and retain native memory checkpoints')
args = parser.parse_args()
root = args.output.resolve(); root.mkdir(parents=True, exist_ok=False)
plugins = root / 'UserPlugins'; plugins.mkdir()
for source in [args.extension] + ([args.keyboard_probe] if args.keyboard_probe else []):
    target = plugins / source.name; shutil.copy2(source, target)
    if sys.platform == 'darwin': subprocess.run(['codesign', '--force', '--sign', '-', str(target)], check=True, capture_output=True)
for helper in args.extension.parent.glob('reawebapi-webview-*'): shutil.copy2(helper, plugins / helper.name)
if sys.platform.startswith('linux'):
    for runtime in args.extension.parent.glob('reawebapi-webkit-*'):
        if runtime.is_dir(): (plugins / runtime.name).symlink_to(runtime.resolve(), target_is_directory=True)
if args.keyboard_probe: (root / 'keyboard-test.enabled').touch()
app = root / 'Scripts/Emulator'; app.mkdir(parents=True)
def unused_cores(directory, names):
    if Path(directory).name != 'cores': return []
    return [name for name in names if name.endswith(('.data','-assets.zip')) and not name.startswith(args.core + '-')]
shutil.copytree(args.emulator / 'vendor/emulatorjs', app / 'vendor/emulatorjs', ignore=unused_cores)
(app / 'test.com').write_bytes(dos_program()); (app / 'palette.nes').write_bytes(nes_program())
game = 'test.com' if args.core == 'dosbox_pure' else 'palette.nes'
if args.core == 'ppsspp':
    assert args.psp_program, '--psp-program is required'
    game = args.psp_program.name; shutil.copy2(args.psp_program, app / game)
(app / 'app.json').write_text(json.dumps({'id': 'emulator-webkit-acceptance', 'webRuntime': {'crossOriginIsolation': 'required'}}))
worker_monitor = '''
// Preserve rendered pixels only in this fixture so asynchronous WebKit readback is deterministic.
const acceptanceGetContext = HTMLCanvasElement.prototype.getContext;
HTMLCanvasElement.prototype.getContext = function(type, attributes) {
  return acceptanceGetContext.call(this, type, /^webgl2?$|^experimental-webgl$/.test(type) ?
    {...attributes, preserveDrawingBuffer:true} : attributes);
};
const acceptanceWorkers = {measurement:'constructed-minus-explicitly-terminated', active:0, created:0, terminated:0, peak:0};
const AcceptanceWorker = window.Worker;
window.Worker = class extends AcceptanceWorker {
  constructor(...args) { super(...args); acceptanceWorkers.created++; acceptanceWorkers.active++;
    acceptanceWorkers.peak=Math.max(acceptanceWorkers.peak,acceptanceWorkers.active); this.acceptanceAlive=true; }
  terminate() { if(this.acceptanceAlive){this.acceptanceAlive=false;acceptanceWorkers.active--;acceptanceWorkers.terminated++;} return super.terminate(); }
};
window.acceptanceWorkers=acceptanceWorkers;
'''
script = Path(__file__).with_name('isolation_emulator.js').read_text()
script = script.replace('reaper.ShowConsoleMsg', 'reaper.host.send')
script = script.replace('diagnostics: await reaper.debug.getDiagnostics()', 'workers: {...acceptanceWorkers}, diagnostics: await reaper.debug.getDiagnostics()')
(app / 'test.js').write_text(worker_monitor + script)
if sys.platform.startswith('linux'):
    with (app / 'test.js').open('a') as stream:
        stream.write('''
window.EJS_ready = async () => {
  const button = document.querySelector('.ejs_start_button');
  Object.assign(button.style,{position:'fixed',top:'200px',left:'200px',bottom:'auto',right:'auto',transform:'none'});
  const bounds = button.getBoundingClientRect();
  await reaper.host.send(`CORE_START:${Math.round(bounds.x+bounds.width/2)},${Math.round(bounds.y+bounds.height/2)}\\n`);
};
''')
settings = {'EJS_player': '#game', 'EJS_core': args.core, 'EJS_gameUrl': game, 'EJS_pathtodata': 'vendor/emulatorjs/data/',
    'EJS_threads': not args.single_thread, 'EJS_startOnLoaded': not sys.platform.startswith('linux'), 'EJS_DEBUG_XX': True, 'EJS_disableAutoLang': True,
    'EJS_language': 'en-US', 'EJS_disableLocalStorage': True, 'acceptancePspScenario': args.psp_scenario}
(app / 'index.html').write_text('<!doctype html><meta charset="utf-8"><title>Core acceptance</title><div id="game" style="width:640px;height:480px"></div><script>' +
    ''.join('window.' + k + '=' + json.dumps(v) + ';' for k, v in settings.items()) +
    f'window.acceptanceSoakSeconds={args.soak_seconds};' + '</script><script src="test.js"></script><script src="vendor/emulatorjs/data/loader.js"></script>')
runner = r'''
local r=reaper
local root=r.GetResourcePath():gsub('\\','/')
local out=assert(io.open(root..'/events.jsonl','w'))
local function status(s) local f=assert(io.open(root..'/status.txt','w'));f:write(s);f:close() end
local function wait(t) local endtime=r.time_precise()+t;repeat coroutine.yield() until r.time_precise()>=endtime end
local co=coroutine.create(function()
  for cycle=0,REOPENS do
    if cycle==1 then
      local path=root..'/Scripts/Emulator/index.html'
      local f=assert(io.open(path,'r'));local html=f:read('*a');f:close()
      f=assert(io.open(path,'w'));f:write((html:gsub('window.acceptanceSoakSeconds=%d+', 'window.acceptanceSoakSeconds=0')));f:close()
    end
    status('cycle '..cycle)
    local id=r.ReaWeb_Open(root..'/Scripts/Emulator/index.html');assert(id>0,r.ReaWeb_GetLastError())
    local deadline=r.time_precise()+100+SOAK
    local finished=false
    repeat
      local message=r.ReaWeb_Receive(id)
      if message and message~='' then
        if message:find('CORE_KEY:',1,true)==1 or message:find('CORE_START:',1,true)==1 then assert(r.ReaWeb_Focus(id));wait(.1) end
        out:write(message);out:flush()
        if message:find('CORE_REPORT:',1,true)==1 then
          assert(message:match('^CORE_REPORT:.-"passed":(%a+)')=='true','Core behavior failed, see events.jsonl')
          finished=true
        end
      end
      wait(.01)
    until finished or r.time_precise()>deadline
    assert(finished,'Core acceptance timed out')
    local start=r.time_precise();assert(r.ReaWeb_Close(id));local elapsed=r.time_precise()-start
    out:write('CORE_CLOSED:{"cycle":'..cycle..',"seconds":'..elapsed..'}\n');out:flush()
    wait(1)
    assert(not r.ReaWeb_IsOpen(id),'Window did not close')
  end
  out:close();status('complete');r.Main_OnCommand(40004,0)
end)
local function step()
  local ok,err=coroutine.resume(co)
  if not ok then status('ERROR '..tostring(err));out:close();return end
  if coroutine.status(co)~='dead' then r.defer(step) end
end
step()
'''
(root / 'runner.lua').write_text(runner.replace('REOPENS', str(args.reopen)).replace('SOAK', str(args.soak_seconds)))
(root / 'EmptyPlugins').mkdir()
(root / 'reaper.ini').write_text('[reaper]\nverchk=0\nloadlastproj=0\nshowlastproj=0\nsplash=0\nhasrecentlyopened=1\nnewprojdo=0\nvst_scan=0\nvstfullstate=49989\nvstpath=' + str(root / 'EmptyPlugins') + '\n[audioconfig]\nmode=4\n')
if sys.platform.startswith('linux'):
    config=root/'reaper.ini';config.write_text(config.read_text().replace('[audioconfig]\nmode=4\n','audiomode=4\n'))
(root / 'ReaWebAPI.ini').write_text('[ReaWebAPI]\nIdleWebViews=0\n[Diagnostics]\nRuntimeDebug=1\n')
mac_keys = {65:0, 66:11, 88:7, 90:6, 13:36, 32:49}
def mac_command(op, **values):
    reply = root / 'keyboard-reply.json'; reply.unlink(missing_ok=True)
    temporary = root / 'keyboard-command.tmp'; temporary.write_text(json.dumps({'op':op, **values}))
    temporary.replace(root / 'keyboard-command.json')
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if reply.exists():
            result = json.loads(reply.read_text()); assert not isinstance(result, dict) or 'error' not in result, result
            return result
        time.sleep(.01)
    raise TimeoutError('Native keyboard command: ' + op)

def dismiss_linux_test_dialogs(pid):
    for title in ('^About REAPER', '^Error opening devices'):
        result=subprocess.run(['xdotool','search','--all','--onlyvisible','--pid',str(pid),'--name',title],capture_output=True,text=True)
        for window in result.stdout.split():
            measured=subprocess.run(['xdotool','getwindowgeometry','--shell',window],capture_output=True,text=True)
            if measured.returncode: continue
            geometry=dict(line.split('=',1) for line in measured.stdout.splitlines() if '=' in line)
            if abs(int(geometry['X']))>10000 or abs(int(geometry['Y']))>10000: continue
            subprocess.run(['xdotool','mousemove','--window',window,str(int(geometry['WIDTH'])-85),str(int(geometry['HEIGHT'])-27),'click','1'],check=True,timeout=2)

with (root / 'process.log').open('w') as log:
    process = subprocess.Popen([str(args.reaper.resolve()), '-newinst', '-cfgfile', str(root / 'reaper.ini'), '-ignoreerrors', '-nosplash', str(root / 'runner.lua')], stdout=log, stderr=log)
    awake = subprocess.Popen(['caffeinate', '-diu', '-t', str(120 * (args.reopen + 1) + args.soak_seconds), '-w', str(process.pid)]) if sys.platform == 'darwin' else None
    try:
        deadline = time.monotonic() + 110 * (args.reopen + 1) + args.soak_seconds
        cursor, focused, last_sample, last_dialog = 0, False, 0, 0
        memory_checkpoint = 0
        reports, samples = [], []
        while time.monotonic() < deadline:
            if sys.platform.startswith('linux') and not focused and time.monotonic()-last_dialog>1:
                dismiss_linux_test_dialogs(process.pid);last_dialog=time.monotonic()
            status = (root / 'status.txt').read_text() if (root / 'status.txt').exists() else ''
            if status.startswith('ERROR'): raise AssertionError(status)
            if (root / 'events.jsonl').exists():
                with (root / 'events.jsonl').open() as stream:
                    stream.seek(cursor); lines = []
                    while True:
                        line = stream.readline()
                        if not line.endswith('\n'): break
                        lines.append(line); cursor = stream.tell()
                for line in lines:
                    if line.startswith('CORE_CLOSED:'): focused = False
                    elif line.startswith('CORE_REPORT:'): reports.append(json.loads(line[12:]))
                    elif line.startswith('CORE_START:'):
                        x,y=line[11:].strip().split(',')
                        windows=subprocess.check_output(['xdotool','search','--all','--pid',str(process.pid),'--name','^Core acceptance$'],text=True).split()
                        subprocess.run(['xdotool','windowraise',windows[-1]],check=True)
                        window=subprocess.check_output(['xdotool','getwindowfocus'],text=True).strip()
                        subprocess.run(['xdotool','mousemove','--window',window,x,y,'click','1'],check=True,timeout=2)
                        focused=True
                    elif line.startswith('CORE_KEY:'):
                        key, down = map(int, line[9:].strip().split(','))
                        if sys.platform == 'darwin':
                            assert args.keyboard_probe, '--keyboard-probe is required on macOS'
                            if not focused: assert mac_command('select'); mac_command('focus', content=True); focused=True
                            mac_command('key', key=mac_keys[key], down=bool(down))
                        else:
                            focused=True
                            native_focus=subprocess.run(['xdotool','getwindowfocus','getwindowname'],capture_output=True,text=True)
                            with (root/'native-input.jsonl').open('a') as trace:
                                trace.write(json.dumps({'key':key,'down':down,'focus':native_focus.stdout,'error':native_focus.stderr})+'\n')
                            symbol = {13:'Return',32:'space'}.get(key,chr(key).lower())
                            subprocess.run(['xdotool','keydown' if down else 'keyup',symbol],check=True)
            if time.monotonic() - last_sample > 10:
                last_sample=time.monotonic()
                row={'seconds':time.monotonic(), 'hostPid':process.pid}
                if sys.platform == 'darwin' and focused:
                    row['native']=mac_command('state')
                    row['workers']=mac_command('eval',script='window.acceptanceWorkers')
                    web_pid=row['native'].get('webProcessPid')
                    if web_pid: row['webRssKiB']=subprocess.check_output(['ps','-o','rss=','-p',str(web_pid)],text=True).strip()
                    if args.memory_diagnostics and web_pid and time.monotonic() >= memory_checkpoint:
                        memory_checkpoint = time.monotonic() + 600
                        snapshot=subprocess.run(['vmmap','-summary',str(web_pid)],capture_output=True,text=True,timeout=15)
                        (root/f'vmmap-{web_pid}-{len(samples):04}.txt').write_text(snapshot.stdout+snapshot.stderr)
                row['hostRssKiB']=subprocess.check_output(['ps','-o','rss=','-p',str(process.pid)],text=True).strip()
                if sys.platform.startswith('linux'):
                    processes=[]
                    for entry in subprocess.check_output(['ps','-eo','pid=,ppid=,rss=,comm='],text=True).splitlines():
                        pid,parent,rss,name=entry.split(None,3)
                        processes.append({'pid':int(pid),'parent':int(parent),'rssKiB':int(rss),'name':name})
                    descendants={process.pid}
                    for _ in range(8):
                        found={item['pid'] for item in processes if item['parent'] in descendants}
                        if found.issubset(descendants): break
                        descendants.update(found)
                    row['processes']=[item for item in processes if item['pid'] in descendants]
                    if args.memory_diagnostics:
                        for item in row['processes']:
                            try:
                                fields={line.split(':',1)[0]:int(line.split()[1]) for line in
                                    Path(f"/proc/{item['pid']}/smaps_rollup").read_text().splitlines()[1:]}
                                item['pssKiB']=fields['Pss']
                                item['privateKiB']=fields['Private_Clean']+fields['Private_Dirty']
                                item['swapKiB']=fields['Swap']
                            except (FileNotFoundError,ProcessLookupError,PermissionError): pass
                samples.append(row); (root/'resources.json').write_text(json.dumps(samples,indent=2))
            if status=='complete': break
            if process.poll() is not None: raise RuntimeError('REAPER exited before completion')
            time.sleep(.02)
        else: raise TimeoutError('Core test deadline')
        process.wait(timeout=15)
        assert len(reports)==args.reopen+1 and all(row['passed'] for row in reports), reports
        (root/'report.json').write_text(json.dumps(reports,indent=2))
        if sys.platform.startswith('linux'):
            observed={item['pid']:item['name'] for row in samples for item in row.get('processes',[]) if item['pid']!=process.pid}
            def still_running(pid, name):
                try:
                    return Path(f'/proc/{pid}/comm').read_text().strip()==name and Path(f'/proc/{pid}/stat').read_text().split(') ',1)[1][0]!='Z'
                except FileNotFoundError: return False
            remaining=[]
            for _ in range(50):
                remaining=[pid for pid,name in observed.items() if still_running(pid,name)]
                if not remaining: break
                time.sleep(.1)
            (root/'cleanup.json').write_text(json.dumps({'remainingSampledChildren':remaining}))
            assert not remaining, f'Test child processes remain: {remaining}'
        print(json.dumps({'passed':True,'core':args.core,'reports':len(reports),'soakSeconds':args.soak_seconds,'workerPeaks':[row['workers']['peak'] for row in reports]}))
    finally:
        if process.poll() is None: process.terminate(); process.wait(timeout=10)
        if awake: awake.wait(timeout=5)
