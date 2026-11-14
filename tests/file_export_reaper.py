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
parser.add_argument('--license', type=Path)
parser.add_argument('--mode', choices=('off','required'), default='off')
args = parser.parse_args()
root = args.output.resolve()
root.mkdir(parents=True, exist_ok=False)
for directory in ('Plugins', 'UserPlugins', 'Scripts'):
    (root / directory).mkdir()
if sys.platform == 'win32':
    for name in ('reaper.exe', 'reaper-license.rk', 'reaper-install-rev.txt'):
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
if args.license: shutil.copy2(args.license, root / 'reaper-license.rk')
(root / 'EmptyPlugins').mkdir()
(root / 'reaper.ini').write_text('[REAPER]\nvstpath64=' + str(root / 'EmptyPlugins') + '\nhasrecentlyopened=1\nvstfullstate=49989\nerrnowarn=5\nwnd_state=0\nshowmaintrack=0\nverchk=0\nloadlastproj=0\n[audioconfig]\nmode=4\n')
if sys.platform != 'win32':
    config = root / 'reaper.ini'
    text = config.read_text().replace('[REAPER]', '[reaper]\nvstpath=' + str(root / 'EmptyPlugins'))
    if sys.platform == 'darwin':
        text = text.replace('[reaper]', '[reaper]\naudiomode=4').replace('[audioconfig]\nmode=4', '[audioconfig]\nmode=0')
        license = Path.home() / 'Library/Application Support/REAPER/reaper-license.rk'
        if license.is_file(): shutil.copy2(license, root / license.name)
    else:
        text = text.replace('[reaper]', '[reaper]\nlinux_audio_mode=3\nlinux_audio_srate=48000\nlinux_audio_bsize=512\nlinux_audio_nch_out=2')
    config.write_text(text)
(root / 'ReaWebAPI.ini').write_text('[ReaWebAPI]\nIdleWebViews=0\n')
app = root / 'Scripts/Export'
app.mkdir()
(app / 'app.json').write_text(json.dumps({'name':'File export acceptance','webRuntime':{'crossOriginIsolation':args.mode}}))
(app / 'index.html').write_text('<!doctype html><meta charset="utf-8"><title>File export acceptance</title><pre id="result">Running</pre><script type="module" src="app.js"></script>')
shutil.copyfile(Path(__file__).resolve().parents[1] / 'runtime/browser-files.mjs', app / 'browser-files.mjs')
(app / 'app.js').write_text(r'''import {saveBlob,installDownloadHandler} from './browser-files.mjs';
const check=(v,m)=>{if(!v)throw Error(m);};
const rejects=async(fn,code)=>{try{await fn();}catch(e){check(e.code===code,`${code}: ${e.code} ${e.message}`);return;}throw Error('Expected '+code);};
try {
  await reaper.lifecycle.ready;
  if(sessionStorage.getItem('reload')) {
    check(!(await reaper.fs.stat('orphan.bin')).exists,'orphan published');
    const writer=await reaper.fs.openWrite('after-reload.bin');
    await writer.write(new Uint8Array([7,8,9]));await writer.commit();
    await reaper.host.send(JSON.stringify({passed:true,reload:true}));
  } else {
    const caps=await reaper.system.getCapabilities();check(caps.runtime.fs.streamingWrite,'missing capability');
    await reaper.fs.writeText('replace.bin','original',{overwrite:true});
    let writer=await reaper.fs.openWrite('replace.bin',{overwrite:true,expectedBytes:4});
    await rejects(()=>writer.write(new Uint8Array([1]),{offset:1}),'WRITE_OFFSET_MISMATCH');
    check(await reaper.fs.readText('replace.bin')==='original','early replacement');
    await writer.write(new Uint8Array([0,255,13,10]));await writer.commit();
    check((await writer.abort()).state==='committed','committed abort');
    check(JSON.stringify(Array.from(await reaper.fs.readBinary('replace.bin')))==='[0,255,13,10]','bytes');
    writer=await reaper.fs.openWrite('cancelled.bin');await writer.write(new Uint8Array([1]));await writer.abort();
    check(!(await reaper.fs.stat('cancelled.bin')).exists,'cancelled published');
    const size=64*1024*1024+1, part=new Uint8Array(1024*1024);for(let i=0;i<part.length;i++)part[i]=i%251;
    const blob=new Blob([...Array(64).fill(part),new Uint8Array([123])]);
    blob.arrayBuffer=()=>{throw Error('whole Blob read');};
    const host={fs:reaper.fs,system:reaper.system,dialog:{saveFile:async()=> 'large.bin'}};
    let progress=0;await saveBlob(blob,'large.bin',host,{onProgress:p=>{check(p.bytes>=progress,'progress order');progress=p.bytes;}});
    check(progress===size,'wrong length');
    host.dialog.saveFile=async()=> 'blob-link.bin';
    await new Promise((resolve,reject)=>{
      const timer=setTimeout(()=>{dispose();reject(Error('Blob link timeout'));},60000);
      const dispose=installDownloadHandler({host,onError:e=>{clearTimeout(timer);dispose();reject(e);},onProgress:p=>{if(p.state==='committed'){clearTimeout(timer);dispose();resolve();}}});
      const url=URL.createObjectURL(new Blob(Array(17).fill(part))),anchor=document.createElement('a');
      anchor.download='blob-link.bin';anchor.href=url;document.body.append(anchor);anchor.click();anchor.remove();URL.revokeObjectURL(url);
    });
    await new Promise(resolve=>setTimeout(resolve,0));
    const abort=new AbortController();host.dialog.saveFile=async()=> 'sdk-cancel.bin';
    await rejects(()=>saveBlob(blob,'x',host,{signal:abort.signal,onProgress:p=>{if(p.state==='writing')abort.abort();}}),'WRITE_CANCELLED');
    check(!(await reaper.fs.stat('sdk-cancel.bin')).exists,'SDK cancel published');
    await rejects(()=>reaper.fs.writeBinary('old-too-large.bin',new Uint8Array(16*1024*1024+1)),'BUFFER_LIMIT');
    writer=await reaper.fs.openWrite('orphan.bin');await writer.write(new Uint8Array([1,2,3]));
    await reaper.host.send(JSON.stringify({passed:true,bytes:progress,capability:caps.runtime.fs.streamingWrite,hostVersion:caps.version,reaper:await reaper.GetAppVersion()}));
    sessionStorage.setItem('reload','1');await reaper.window.reload();
  }
} catch(e) {await reaper.host.send(JSON.stringify({passed:false,error:String(e),code:e.code,stack:e.stack}));}
''',encoding='utf-8')
(root / 'Scripts/runner.lua').write_text('''
local r = reaper
local root = r.GetResourcePath():gsub('\\\\','/')
local out = assert(io.open(root..'/result.jsonl','w'))
local id = r.ReaWeb_Open(root..'/Scripts/Export/index.html')
assert(id > 0, r.ReaWeb_GetLastError())
local count, ticks, last, maxGap, start = 0, 0, r.time_precise(), 0, r.time_precise()
local function step()
  local now = r.time_precise()
  ticks = ticks + 1
  maxGap = math.max(maxGap, now - last)
  last = now
  local message = r.ReaWeb_Receive(id)
  if message and message ~= '' then out:write(message..'\\n') out:flush() count = count + 1 if message:find('"passed":false', 1, true) then count = 2 end end
  if count >= 2 or now-start > 240 then
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
    startup.wShowWindow = 1
executable = root / 'reaper.exe' if sys.platform == 'win32' else args.source.resolve()
process = subprocess.Popen([str(executable), '-newinst', '-cfgfile', str(root / 'reaper.ini'), '-nosplash', str(root / 'Scripts/runner.lua')], cwd=root, startupinfo=startup)
awake = subprocess.Popen(['caffeinate', '-diu', '-w', str(process.pid)]) if sys.platform == 'darwin' else None
try:
    deadline = time.monotonic() + 260
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
import hashlib
payload = app / 'large.bin'
expected = hashlib.sha256()
block = bytes(i % 251 for i in range(1024 * 1024))
for _ in range(64): expected.update(block)
expected.update(bytes([123]))
actual = hashlib.sha256()
with payload.open('rb') as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b''): actual.update(block)
assert actual.digest() == expected.digest(), 'Large file hash mismatch'
assert (app / 'blob-link.bin').stat().st_size == 17 * 1024 * 1024
expected_link = hashlib.sha256(bytes(i % 251 for i in range(1024 * 1024)) * 17).hexdigest()
with (app / 'blob-link.bin').open('rb') as stream:
    assert hashlib.file_digest(stream, 'sha256').hexdigest() == expected_link
assert not list(app.glob('.reaweb-write-*')), 'Leaked staging directory'
assert (app / 'after-reload.bin').read_bytes() == bytes([7,8,9])
print(json.dumps({'passed':True,'mode':args.mode,'sha256':actual.hexdigest(),'bytes':payload.stat().st_size,'reports':rows}))
