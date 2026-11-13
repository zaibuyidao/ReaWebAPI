"""Production extension and real WebView2 acceptance, using the mock REAPER ABI host."""
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import shutil
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import threading


def run_windows(host):
    root, user32 = host['root'], host['user32']
    source = Path(__file__).resolve().parents[1] / 'examples/runtime-features/isolation'
    app = root / 'Scripts/Isolation'
    shutil.copytree(source, app, dirs_exist_ok=True)
    manifest = json.loads((app / 'app.json').read_text())
    manifest['id'] = 'isolation-acceptance'
    (app / 'app.json').write_text(json.dumps(manifest))
    with (app / 'app.js').open('a') as out:
        out.write("\nwindow.isolationCheck.then(report=>reaper.ShowConsoleMsg('ISOLATION:' + JSON.stringify(report) + '\\n'),error=>reaper.ShowConsoleMsg('ISOLATION_ERROR:' + String(error) + '\\n'));\n")
    diagnostics = C.CFUNCTYPE(C.c_char_p, C.c_int)(host['registrations'][b'API_ReaWeb_GetDiagnostics'])
    user32.PeekMessageW.argtypes = [C.POINTER(W.MSG), W.HWND, W.UINT, W.UINT, W.UINT]
    user32.TranslateMessage.argtypes = [C.POINTER(W.MSG)]
    user32.DispatchMessageW.argtypes = [C.POINTER(W.MSG)]

    def pump():
        msg = W.MSG()
        for _ in range(100):
            if not user32.PeekMessageW(C.byref(msg), None, 0, 0, 1): break
            user32.TranslateMessage(C.byref(msg)); user32.DispatchMessageW(C.byref(msg))
        host['timer']()
        time.sleep(.005)

    def settle(seconds=.2):
        end = time.monotonic() + seconds
        while time.monotonic() < end: pump()

    def launch(folder, prefix='ISOLATION:'):
        host['messages'].clear()
        identifier = host['open_window'](str(folder / 'index.html').encode())
        assert identifier, host['get_error']()
        end = time.monotonic() + 30
        while time.monotonic() < end and not any(m.startswith(prefix) or m.startswith('ISOLATION_ERROR:') for m in host['messages']): pump()
        values = [m for m in host['messages'] if m.startswith(prefix)]
        assert values, (host['messages'], diagnostics(identifier))
        return identifier, json.loads(values[-1][len(prefix):])

    ordinary = root / 'Scripts/Ordinary'
    ordinary.mkdir(parents=True, exist_ok=True)
    (ordinary / 'app.json').write_text(json.dumps({'id': 'isolation-ordinary'}))
    (ordinary / 'index.html').write_text('''<!doctype html><script>
    reaper.lifecycle.ready.then(async()=>reaper.ShowConsoleMsg('ORDINARY:'+JSON.stringify({
      capabilities:{webRuntime:(await reaper.system.getCapabilities()).webRuntime},visits:localStorage.getItem('isolationVisits')})+'\\n'));
    </script>''')
    normal_id, normal = launch(ordinary, 'ORDINARY:')
    assert normal['visits'] is None and normal['capabilities']['webRuntime']['crossOriginIsolation']['requested'] == 'off'
    assert normal['capabilities']['webRuntime']['crossOriginIsolation']['active'] is False
    assert normal['capabilities']['webRuntime']['browser']['worker'] is True
    reports = []
    concurrent = []
    for mode in ('required', 'off'):
        manifest['webRuntime']['crossOriginIsolation'] = mode
        (app / 'app.json').write_text(json.dumps(manifest))
        identifier, report = launch(app)
        concurrent.append((identifier, report))
        assert report['diagnostics']['webRuntime']['crossOriginIsolation']['requested'] == 'required'
    assert [r['visits'] for _, r in concurrent] == [1, 2]
    for identifier, _ in concurrent: assert host['close_window'](identifier)
    settle()
    manifest['webRuntime']['crossOriginIsolation'] = 'required'
    (app / 'app.json').write_text(json.dumps(manifest))
    for index in range(20):
        started = time.monotonic()
        identifier, report = launch(app)
        assert report['passed'] and report['visits'] == index + 3 and report['indexedDBVisits'] == index + 3, report
        assert report['origin'] == 'reaweb://isolation-acceptance'
        assert report['diagnostics']['webRuntime']['crossOriginIsolation']['requested'] == 'required'
        report['openSeconds'] = time.monotonic() - started
        started = time.monotonic()
        assert host['close_window'](identifier)
        settle()
        report['closeSeconds'] = time.monotonic() - started
        reports.append(report)
    assert host['close_window'](normal_id)
    settle()
    # A policy change takes effect only after the final window closes, without changing storage identity.
    manifest['webRuntime']['crossOriginIsolation'] = 'off'
    (app / 'app.json').write_text(json.dumps(manifest))
    (app / 'index.html').write_text((ordinary / 'index.html').read_text())
    identifier, off = launch(app, 'ORDINARY:')
    assert off['visits'] == '22' and off['capabilities']['webRuntime']['origin'] == reports[-1]['origin']
    assert off['capabilities']['webRuntime']['crossOriginIsolation']['active'] is False
    assert host['close_window'](identifier)
    settle()
    class ResourceHandler(BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_GET(self):
            body = b'window.remoteLoaded = (window.remoteLoaded || 0) + 1;'
            self.send_response(200)
            self.send_header('Content-Type', 'text/javascript')
            self.send_header('Content-Length', str(len(body)))
            if self.path.startswith('/corp'): self.send_header('Cross-Origin-Resource-Policy', 'cross-origin')
            if self.path.startswith('/cors'): self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers(); self.wfile.write(body)
    server = ThreadingHTTPServer(('127.0.0.1', 0), ResourceHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    network = root / 'Scripts/IsolationNetwork'
    network.mkdir(parents=True, exist_ok=True)
    (network / 'app.json').write_text(json.dumps({'id': 'isolation-network', 'webRuntime': {'crossOriginIsolation': 'required'}}))
    url = f'http://127.0.0.1:{server.server_port}'
    (network / 'index.html').write_text('''<!doctype html><script>
    reaper.lifecycle.ready.then(async()=>{
      const base=BASE;
      const script=(path,cors)=>new Promise(resolve=>{const s=document.createElement('script');s.src=base+path;if(cors)s.crossOrigin='anonymous';s.onload=()=>resolve(true);s.onerror=()=>resolve(false);document.head.append(s)});
      const report={corp:await script('/corp.js'),blocked:await script('/blocked.js'),cors:await script('/cors.js',true)};
      report.corsFetch=await fetch(base+'/cors-data').then(r=>r.ok,()=>false);
      report.deniedFetch=await fetch(base+'/blocked-data').then(()=>true,()=>false);
      report.coepFetch=await fetch(base+'/blocked-data',{mode:'no-cors'}).then(()=>true,()=>false);
      report.loads=window.remoteLoaded;
      await reaper.ShowConsoleMsg('NETWORK:'+JSON.stringify(report)+'\\n');
    });</script>'''.replace('BASE', json.dumps(url)))
    try:
        identifier, network_report = launch(network, 'NETWORK:')
        assert network_report == {'corp': True, 'blocked': False, 'cors': True, 'corsFetch': True, 'deniedFetch': False, 'coepFetch': False, 'loads': 2}, network_report
        assert host['close_window'](identifier)
        settle()
    finally:
        server.shutdown(); server.server_close()
    blocked = root / 'Scripts/IsolationCSP'
    blocked.mkdir(parents=True, exist_ok=True)
    (blocked / 'app.json').write_text(json.dumps({'id': 'isolation-csp', 'webRuntime': {'crossOriginIsolation': 'required'}}))
    (blocked / 'index.html').write_text('''<!doctype html><meta http-equiv="Content-Security-Policy" content="script-src 'self' 'unsafe-inline'; worker-src 'none'"><script>
    reaper.lifecycle.ready.then(async()=>{
      let wasm=false;
      try{await WebAssembly.compile(new Uint8Array([0,97,115,109,1,0,0,0]));wasm=true}catch{}
      await reaper.ShowConsoleMsg('CSP:'+JSON.stringify({wasm,browser:(await reaper.system.getCapabilities()).webRuntime.browser})+'\\n');
    });</script>''')
    identifier, csp_report = launch(blocked, 'CSP:')
    assert csp_report['wasm'] is False and csp_report['browser']['worker'] is False and csp_report['browser']['blobWorker'] is False, csp_report
    assert host['close_window'](identifier)
    settle()
    result = {'ordinary': normal, 'multiWindow': [r for _, r in concurrent], 'isolated': reports, 'offAfterReopen': off, 'network': network_report, 'csp': csp_report}
    (root / 'isolation-report.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print('PASS: URL/Blob Workers, four-thread atomic WASM, SAB clone, wait/notify, errors, 20 reopens, storage and ordinary App coexistence')
