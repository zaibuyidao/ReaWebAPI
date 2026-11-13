"""Opt-in real REAPER isolation, storage and reopen acceptance for WebKit backends."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread

PAGE = '''<!doctype html><title>Isolation acceptance</title><script>
const visits=Number(localStorage.getItem('executed') || 0) + 1;
localStorage.setItem('executed', String(visits));
(async () => {
  await reaper.lifecycle.ready;
  const {webRuntime,version:hostVersion} = await reaper.system.getCapabilities();
  const capabilities = {webRuntime,version:hostVersion};
  await reaper.host.send(JSON.stringify({capabilities, visits,
    version:await reaper.GetAppVersion()}));
})();
</script>'''

RUNNER = r'''
local r = reaper
local root = r.GetResourcePath():gsub('\\','/')
assert(root:find('isolation%-acceptance'), 'Disposable acceptance profile required')
local out = assert(io.open(root..'/result.jsonl','w'))
local function status(value) local f=assert(io.open(root..'/status.txt','w'));f:write(value);f:close() end
local function wait(seconds) local untiltime=r.time_precise()+seconds; repeat coroutine.yield() until r.time_precise()>=untiltime end
local co=coroutine.create(function()
  assert(r.APIExists('ReaWeb_Open'), 'Extension not loaded')
  for cycle=0,20 do
    for _,mode in ipairs({'off','required'}) do
      status(mode..':'..cycle)
      local id=r.ReaWeb_Open(root..'/Scripts/'..mode..'/index.html')
      assert(id>0,r.ReaWeb_GetLastError())
      local deadline=r.time_precise()+15
      local diagnostic,message
      repeat
        wait(.05)
        diagnostic=r.ReaWeb_GetDiagnostics(id)
        message=r.ReaWeb_Receive(id)
      until ((mode=='off' or EXPECT_ISOLATED) and message and message~='') or
        (mode=='required' and not EXPECT_ISOLATED and diagnostic:find('CROSS_ORIGIN_ISOLATION_FAILED')) or r.time_precise()>deadline
      assert(r.time_precise()<=deadline,'Initialization timed out: '..mode)
      if mode=='required' and not EXPECT_ISOLATED then
        assert(not r.ReaWeb_IsReady(id),'Required App unexpectedly ready')
        assert(not message or message=='','Required App executed')
        message='null'
      end
      out:write('{"cycle":'..cycle..',"mode":"'..mode..'","page":'..message..',"diagnostics":'..diagnostic..'}\n');out:flush()
      if r.ReaWeb_IsOpen(id) then assert(r.ReaWeb_Close(id),r.ReaWeb_GetLastError()) end
      wait(.1)
      assert(not r.ReaWeb_IsOpen(id),'Window did not close')
    end
  end
  if EXPECT_ISOLATED then
    local function receive(id)
      assert(id>0,r.ReaWeb_GetLastError())
      local deadline=r.time_precise()+15
      repeat
        local message=r.ReaWeb_Receive(id)
        if message and message~='' then return message end
        wait(.05)
      until r.time_precise()>deadline
      error('Policy acceptance timed out')
    end
    local f=assert(io.open(root..'/multi-window.jsonl','w'))
    local ids={}
    for _,mode in ipairs({'required','required','off'}) do
      local id=r.ReaWeb_Open(root..'/Scripts/'..mode..'/index.html');ids[#ids+1]=id
      f:write(receive(id)..'\n')
    end
    f:close()
    for _,id in ipairs(ids) do r.ReaWeb_Close(id) end
    for _,name in ipairs({'network','csp'}) do
      status(name)
      local id=r.ReaWeb_Open(root..'/Scripts/'..name..'/index.html')
      local f=assert(io.open(root..'/'..name..'.json','w'));f:write(receive(id));f:close()
      r.ReaWeb_Close(id)
    end
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

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reaper', type=Path, required=True)
    parser.add_argument('--extension', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expect-isolated', action='store_true')
    args = parser.parse_args()
    root = args.output.resolve()
    assert 'isolation-acceptance' in root.name
    root.mkdir(parents=True, exist_ok=False)
    (root / 'UserPlugins').mkdir()
    extension = root / 'UserPlugins' / args.extension.name
    shutil.copy2(args.extension, extension)
    if sys.platform == 'darwin':
        subprocess.run(['codesign', '--force', '--sign', '-', str(extension)], check=True, capture_output=True)
        license_path = Path.home() / 'Library/Application Support/REAPER/reaper-license.rk'
    else:
        license_path = Path.home() / '.config/REAPER/reaper-license.rk'
        for helper in args.extension.parent.glob('reawebapi-webview-*'):
            shutil.copy2(helper, root / 'UserPlugins' / helper.name)
        for runtime in args.extension.parent.glob('reawebapi-webkit-*'):
            if runtime.is_dir(): (root / 'UserPlugins' / runtime.name).symlink_to(runtime.resolve(), target_is_directory=True)
    if license_path.exists(): shutil.copy2(license_path, root / license_path.name)
    for mode in ('off', 'required'):
        app = root / 'Scripts' / mode
        app.mkdir(parents=True)
        (app / 'app.json').write_text(json.dumps({'id': 'webkit-acceptance-' + mode,
            'webRuntime': {'crossOriginIsolation': mode}}))
        (app / 'index.html').write_text(PAGE)
    server = None
    if args.expect_isolated:
        class ResourceHandler(BaseHTTPRequestHandler):
            def do_GET(self):
                body=b'window.remoteLoaded=(window.remoteLoaded||0)+1;'
                self.send_response(200); self.send_header('Content-Type','text/javascript')
                self.send_header('Content-Length',str(len(body)))
                if self.path.startswith('/corp'): self.send_header('Cross-Origin-Resource-Policy','cross-origin')
                if self.path.startswith('/cors'): self.send_header('Access-Control-Allow-Origin','*')
                self.end_headers(); self.wfile.write(body)
            def log_message(self,*args): pass
        server=ThreadingHTTPServer(('127.0.0.1',0),ResourceHandler)
        Thread(target=server.serve_forever,daemon=True).start()
        pages={
            'network': '''<!doctype html><script>reaper.lifecycle.ready.then(async()=>{
              const base=BASE;
              const script=(path,cors)=>new Promise(resolve=>{const s=document.createElement('script');s.src=base+path;if(cors)s.crossOrigin='anonymous';s.onload=()=>resolve(true);s.onerror=()=>resolve(false);document.head.append(s)});
              const report={corp:await script('/corp.js'),blocked:await script('/blocked.js'),cors:await script('/cors.js',true)};
              report.corsFetch=await fetch(base+'/cors-data').then(r=>r.ok,()=>false);
              report.deniedFetch=await fetch(base+'/blocked-data').then(()=>true,()=>false);
              report.coepFetch=await fetch(base+'/blocked-data',{mode:'no-cors'}).then(()=>true,()=>false);
              report.loads=window.remoteLoaded;await reaper.host.send(JSON.stringify(report));
            });</script>'''.replace('BASE',json.dumps(f'http://127.0.0.1:{server.server_port}')),
            'csp': '''<!doctype html><meta http-equiv="Content-Security-Policy" content="script-src 'self' 'unsafe-inline'; worker-src 'none'"><script>
              reaper.lifecycle.ready.then(async()=>{let compiled=false,wasm=false;try{const module=await WebAssembly.compile(new Uint8Array([0,97,115,109,1,0,0,0]));compiled=true;new WebAssembly.Instance(module);wasm=true}catch{}
              await reaper.host.send(JSON.stringify({compiled,wasm,browser:(await reaper.system.getCapabilities()).webRuntime.browser}));});</script>'''}
        for name,page in pages.items():
            app=root/'Scripts'/name;app.mkdir()
            (app/'app.json').write_text(json.dumps({'id':'webkit-acceptance-'+name,'webRuntime':{'crossOriginIsolation':'required'}}))
            (app/'index.html').write_text(page)
    (root / 'runner.lua').write_text(RUNNER.replace('EXPECT_ISOLATED', str(args.expect_isolated).lower()))
    (root / 'EmptyPlugins').mkdir()
    (root / 'reaper.ini').write_text('[reaper]\nverchk=0\nloadlastproj=0\nshowlastproj=0\nsplash=0\n'
        'hasrecentlyopened=1\nnewprojdo=0\nvst_scan=0\nvstfullstate=49989\nvstpath=' + str(root / 'EmptyPlugins') + '\n[audioconfig]\nmode=4\n')
    (root / 'ReaWebAPI.ini').write_text('[ReaWebAPI]\nIdleWebViews=0\n[Diagnostics]\nRuntimeDebug=1\nAppDebug=0\n')
    with (root / 'process.log').open('w') as log:
        process = subprocess.Popen([str(args.reaper), '-newinst', '-cfgfile', str(root / 'reaper.ini'),
            '-nosplash', str(root / 'runner.lua')], cwd=root, stdout=log, stderr=log)
        awake = subprocess.Popen(['caffeinate', '-diu', '-t', '260', '-w', str(process.pid)]) if sys.platform == 'darwin' else None
        try:
            deadline = time.monotonic() + 240
            while time.monotonic() < deadline:
                status = (root / 'status.txt').read_text() if (root / 'status.txt').exists() else ''
                if status == 'complete': break
                if status.startswith('ERROR'): raise AssertionError(status)
                if process.poll() is not None: raise RuntimeError(f'REAPER exited {process.returncode}: {status}')
                if sys.platform == 'darwin' and not status:
                    # Dismiss only this disposable instance's first-run audio prompt.
                    subprocess.run(['osascript', '-e', f'''tell application "System Events"
                      tell (first process whose unix id is {process.pid})
                        repeat with w in windows
                          if exists button "No" of w then click button "No" of w
                        end repeat
                      end tell
                    end tell'''], capture_output=True, timeout=5)
                time.sleep(.2)
            else: raise TimeoutError('REAPER acceptance timed out: ' + status)
            process.wait(timeout=15)
        finally:
            if process.poll() is None: process.terminate(); process.wait(timeout=10)
            if awake: awake.wait(timeout=5)
            if server: server.shutdown(); server.server_close()
    rows = [json.loads(line) for line in (root / 'result.jsonl').read_text().splitlines()]
    assert len(rows) == 42
    if args.expect_isolated:
        windows=[json.loads(line) for line in (root/'multi-window.jsonl').read_text().splitlines()]
        visits=[row['visits'] for row in windows]
        assert visits==[22,23,22], visits
        origins=[row['capabilities']['webRuntime']['origin'] for row in windows]
        assert origins[0]==origins[1] and origins[0]!=origins[2], origins
        network=json.loads((root/'network.json').read_text())
        assert network=={'corp':True,'blocked':False,'cors':True,'corsFetch':True,'deniedFetch':False,'coepFetch':False,'loads':2}, network
        csp=json.loads((root/'csp.json').read_text())
        assert not csp['wasm'] and csp['browser']['worker'] is False and csp['browser']['blobWorker'] is False, csp
    for row in rows:
        web = row['diagnostics']['webRuntime']
        isolated = args.expect_isolated and row['mode'] == 'required'
        if isolated:
            assert web['origin'].startswith('http://127.0.0.1:') and web['origin'] == rows[1]['diagnostics']['webRuntime']['origin']
            assert web['mode'] == 'app-loopback' and web['localResources'] is True
        else: assert web['origin'] == 'reaweb://webkit-acceptance-' + row['mode']
        assert web['crossOriginIsolation']['active'] is isolated
        assert web['browser']['sharedArrayBuffer'] is isolated
        if row['mode'] == 'required' and not args.expect_isolated:
            assert web['crossOriginIsolation']['failureStage'] == 'document-policy'
            assert row['page'] is None
        else:
            assert row['page']['visits'] == row['cycle'] + 1
            browser = row['page']['capabilities']['webRuntime']['browser']
            assert browser['worker'] is True and browser['blobWorker'] is True
            if isolated:
                assert browser['wasmSharedMemory'] is True
                for worker in browser['workers'].values():
                    assert worker['crossOriginIsolated'] and worker['sharedRoundTrip']
    print(json.dumps({'passed': True, 'reopensPerMode': 20, 'reaperVersion': rows[0]['page']['version'],
        'offBrowser': rows[0]['page']['capabilities']['webRuntime']['browser'],
        'requiredIsolation': rows[1]['diagnostics']['webRuntime']['crossOriginIsolation']}))

if __name__ == '__main__': main()
