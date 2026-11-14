"""Interactive native-dialog acceptance in an isolated REAPER profile.

Save new output.bin, cancel, reject existing.bin replacement and cancel, then
confirm replacement. On Windows, use an output directory outside low-integrity
automation workspaces, for example a new directory under Downloads.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=Path(r'C:\REAPER'))
parser.add_argument('--dll', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--license', type=Path)
parser.add_argument('--prepare-only', action='store_true')
parser.add_argument('--export-directory', type=Path)
parser.add_argument('--mode', choices=('off','required'), default='required')
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
(root / 'reaper.ini').write_text('[REAPER]\nvstpath64=' + str(root / 'EmptyPlugins') + '\nhasrecentlyopened=1\nvstfullstate=49989\nerrnowarn=5\nwnd_state=0\nshowmaintrack=0\nverchk=0\nloadlastproj=0\n[audioconfig]\nmode=4\n', encoding='utf-8')
if sys.platform != 'win32':
    config = root / 'reaper.ini'
    text = config.read_text(encoding='utf-8').replace('[REAPER]', '[reaper]\nvstpath=' + str(root / 'EmptyPlugins'))
    if sys.platform == 'darwin':
        text = text.replace('[reaper]', '[reaper]\naudiomode=4').replace('[audioconfig]\nmode=4', '[audioconfig]\nmode=0')
        license = Path.home() / 'Library/Application Support/REAPER/reaper-license.rk'
        if license.is_file(): shutil.copy2(license, root / license.name)
    else:
        text = text.replace('[reaper]', '[reaper]\nlinux_audio_mode=3\nlinux_audio_srate=48000\nlinux_audio_bsize=512\nlinux_audio_nch_out=2')
    config.write_text(text, encoding='utf-8')
(root / 'ReaWebAPI.ini').write_text('[ReaWebAPI]\nIdleWebViews=0\n')
app=root/'Scripts/Dialog'
app.mkdir()
chosen=args.export_directory.resolve() if args.export_directory else root/'Exports \u6d4b\u8bd5'
chosen.mkdir()
config=root/'reaper.ini'
config.write_text(config.read_text(encoding='utf-8').replace('hasrecentlyopened=1', 'hasrecentlyopened=1\nlastcwd='+str(chosen)), encoding='utf-8')
(chosen/'existing.bin').write_bytes(b'original keep me')
(app/'app.json').write_text(json.dumps({'name':'Native Save Dialog Acceptance','webRuntime':{'crossOriginIsolation':args.mode}}))
(app/'index.html').write_text('<!doctype html><meta charset="utf-8"><title>Native Save Dialog Acceptance</title><pre id="stage"></pre><script type="module" src="app.js"></script>')
shutil.copyfile(Path(__file__).resolve().parents[1]/'runtime/browser-files.mjs',app/'browser-files.mjs')
(app/'paths.json').write_text(json.dumps({'existing':str(chosen/'existing.bin'),'new':str(chosen/'new output.bin')}),encoding='utf-8')
(app/'app.js').write_text(r'''import {saveBlob} from './browser-files.mjs';
const report=async value=>{document.querySelector('#stage').textContent=JSON.stringify(value);await reaper.host.send(JSON.stringify(value));};
const check=(value,message)=>{if(!value)throw Error(message);};
try {
 await reaper.lifecycle.ready;
 const paths=await(await fetch('./paths.json')).json();
 const block=new Uint8Array(1048576);for(let i=0;i<block.length;i++)block[i]=i%251;
 const blob=new Blob(Array(17).fill(block));
 for(const stage of ['save-new','cancel','reject-overwrite','confirm-overwrite']) {
   await report({stage,waiting:true});
   const name=stage==='save-new'?paths.new:stage==='cancel'?'cancel.bin':paths.existing;
   const value=await saveBlob(blob,name);
   if(stage==='save-new')check(value?.replaceAll('\\','/')===paths.new.replaceAll('\\','/'),'Unexpected selected path: '+value);
   else if(stage==='confirm-overwrite')check(value?.replaceAll('\\','/')===paths.existing.replaceAll('\\','/'),'Unexpected overwrite path: '+value);
   else {check(value===null,'Cancel did not return null');check(await reaper.fs.readText(paths.existing)==='original keep me','Original changed before confirmation');}
   await report({stage,passed:true,path:value});
 }
 await report({done:true,passed:true,version:(await reaper.system.getCapabilities()).version});
} catch(e) {await report({done:true,passed:false,error:String(e),stack:e.stack});}
''',encoding='utf-8')
(root/'Scripts/runner.lua').write_text(r'''
local r=reaper
local root=r.GetResourcePath():gsub('\\','/')
local out=assert(io.open(root..'/result.jsonl','w'))
local id=r.ReaWeb_Open(root..'/Scripts/Dialog/index.html')
assert(id>0,r.ReaWeb_GetLastError())
local start=r.time_precise()
local function step()
 local message=r.ReaWeb_Receive(id)
 if message and message~='' then out:write(message..'\n') out:flush() end
 if (message and message:find('"done":true',1,true)) or r.time_precise()-start>1200 then
  out:close() r.ReaWeb_Close(id) r.Main_OnCommand(40004,0)
 else r.defer(step) end
end
r.defer(step)
''',encoding='utf-8')
if args.prepare_only:
    (root/'reaper-kb.ini').write_text('SCR 4 0 RS1111111111111111111111111111111111111111 "Custom: Native dialog acceptance" "runner.lua"\nKEY 1 119 _RS1111111111111111111111111111111111111111 0\n')
    print(str(root));sys.exit(0)
executable=root/'reaper.exe' if sys.platform=='win32' else args.source.resolve()
process=subprocess.Popen([str(executable),'-newinst','-cfgfile',str(root/'reaper.ini'),'-nosplash',str(root/'Scripts/runner.lua')],cwd=chosen)
(root/'process.json').write_text(json.dumps({'pid':process.pid}))
awake=subprocess.Popen(['caffeinate','-diu','-w',str(process.pid)]) if sys.platform=='darwin' else None
try:
 process.wait(timeout=1260)
finally:
 if process.poll() is None:process.terminate();process.wait(timeout=10)
 if awake:awake.wait(timeout=5)
rows=[json.loads(line) for line in (root/'result.jsonl').read_text(encoding='utf-8').splitlines()]
assert rows[-1].get('done') and rows[-1].get('passed'),rows
assert len([r for r in rows if r.get('passed')])==5,rows
import hashlib
expected=hashlib.sha256(bytes(i%251 for i in range(1048576))*17).hexdigest()
for name in ('new output.bin','existing.bin'):
 with (chosen/name).open('rb') as stream:assert hashlib.file_digest(stream,'sha256').hexdigest()==expected
assert sorted(p.name for p in chosen.iterdir())==['existing.bin','new output.bin']
assert not list(root.rglob('.reaweb-write-*'))
print(json.dumps({'passed':True,'bytesPerExport':17*1048576,'sha256':expected,'cases':rows}))
