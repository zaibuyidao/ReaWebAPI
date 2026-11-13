"""Opt-in isolated portable REAPER acceptance. Never modifies the installed application or profile."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=Path(r'C:\REAPER'))
parser.add_argument('--dll', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = args.output.resolve()
root.mkdir(parents=True, exist_ok=False)
for directory in ('Plugins', 'UserPlugins', 'Scripts'):
    (root / directory).mkdir()
if sys.platform == 'win32':
    for name in ('reaper.exe', 'reaper-license.rk'):
        if (args.source / name).exists(): shutil.copyfile(args.source / name, root / name)
    for name in ('jsfx.dll', 'reaper_midi.dll', 'reaper_wave.dll', 'reaper_csurf.dll'):
        shutil.copyfile(args.source / 'Plugins' / name, root / 'Plugins' / name)
shutil.copyfile(args.dll, root / 'UserPlugins' / args.dll.name)
if sys.platform == 'darwin':
    subprocess.run(['codesign', '--force', '--sign', '-', str(root / 'UserPlugins' / args.dll.name)], check=True, capture_output=True)
else:
    for helper in args.dll.parent.glob('reawebapi-webview-*'): shutil.copy2(helper, root / 'UserPlugins' / helper.name)
    if sys.platform.startswith('linux'):
        for runtime in args.dll.parent.glob('reawebapi-webkit-*'):
            if runtime.is_dir(): (root / 'UserPlugins' / runtime.name).symlink_to(runtime.resolve(), target_is_directory=True)
(root / 'reaper.ini').write_text('[REAPER]\nwnd_state=0\nshowmaintrack=0\nverchk=0\nloadlastproj=0\n[audioconfig]\nmode=4\n')
(root / 'ReaWebAPI.ini').write_text('[ReaWebAPI]\nIdleWebViews=0\n')
app = root / 'Scripts/Isolation'
shutil.copytree(Path(__file__).resolve().parents[1] / 'examples/runtime-features/isolation', app)
with (app / 'app.js').open('a', encoding='utf-8') as stream:
    stream.write('''
window.isolationCheck.then(async report => {
  report.reaperVersion = await reaper.GetAppVersion();
  await reaper.host.send(JSON.stringify(report));
  if (!sessionStorage.getItem('reloaded')) {
    sessionStorage.setItem('reloaded', '1');
    await reaper.window.reload();
  }
}, error => reaper.host.send(JSON.stringify({error:String(error)})));
''')
(root / 'Scripts/runner.lua').write_text('''
local r = reaper
local root = r.GetResourcePath():gsub('\\\\','/')
local out = assert(io.open(root..'/result.jsonl','w'))
local id = r.ReaWeb_Open(root..'/Scripts/Isolation/index.html')
assert(id > 0, r.ReaWeb_GetLastError())
local count, ticks, last, maxGap, start = 0, 0, r.time_precise(), 0, r.time_precise()
local function step()
  local now = r.time_precise()
  ticks = ticks + 1
  maxGap = math.max(maxGap, now - last)
  last = now
  local message = r.ReaWeb_Receive(id)
  if message and message ~= '' then out:write(message..'\\n') out:flush() count = count + 1 end
  if count >= 2 or now-start > 45 then
    out:write(string.format('{"hostTicks":%d,"maxGapSeconds":%f,"reports":%d}\\n', ticks, maxGap, count))
    out:close()
    r.ReaWeb_Close(id)
    r.Main_OnCommand(40004, 0)
  else r.defer(step) end
end
r.defer(step)
''', encoding='utf-8')
startup = None
if sys.platform == 'win32':
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
executable = root / 'reaper.exe' if sys.platform == 'win32' else args.source.resolve()
process = subprocess.Popen([str(executable), '-newinst', '-cfgfile', str(root / 'reaper.ini'), '-nosplash', str(root / 'Scripts/runner.lua')], cwd=root, startupinfo=startup)
awake = subprocess.Popen(['caffeinate', '-di', '-w', str(process.pid)]) if sys.platform == 'darwin' else None
try:
    deadline = time.monotonic() + 65
    while process.poll() is None and time.monotonic() < deadline:
        if sys.platform == 'darwin' and not (root / 'result.jsonl').exists():
            subprocess.run(['osascript', '-e', f'''tell application "System Events"
              tell (first process whose unix id is {process.pid})
                repeat with w in windows
                  if exists button "No" of w then click button "No" of w
                end repeat
              end tell
            end tell'''], capture_output=True, timeout=5)
        time.sleep(.2)
    process.wait(timeout=1)
finally:
    if process.poll() is None: process.terminate(); process.wait(timeout=10)
    if awake: awake.wait(timeout=5)
rows = [json.loads(line) for line in (root / 'result.jsonl').read_text().splitlines()]
assert len(rows) == 3 and all(row.get('passed') for row in rows[:2]), rows
assert [row['diagnostics']['documentGeneration'] for row in rows[:2]] == [1, 2], rows
assert [row['visits'] for row in rows[:2]] == [1, 2], rows
print(json.dumps({'reaperVersion': rows[0]['reaperVersion'], 'browserVersion': rows[0]['diagnostics']['browserVersion'], 'host': rows[2], 'reloadPassed': True}))
