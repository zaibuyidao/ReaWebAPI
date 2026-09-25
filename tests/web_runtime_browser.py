"""Actual browser capability/persistence checks; imported by the mock-host driver."""
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import shutil
import time


def run_windows(host):
    import web_runtime_network
    server, endpoints = web_runtime_network.start()
    try:
        run_checks(host, endpoints)
    finally:
        server.shutdown(); server.server_close()


def run_checks(host, endpoints):
    import web_runtime_network
    root, user32 = host['root'], host['user32']
    source = Path(__file__).resolve().parents[1] / 'runtime/web-runtime'
    app_a, app_b = root / 'Scripts/空 格#%/AppA', root / 'Scripts/AppB'
    for folder in (app_a, app_b):
        shutil.copytree(source, folder, dirs_exist_ok=True)
        web_runtime_network.configure(folder, endpoints)
        # Native bridge output is only a transport for the report. The checks
        # themselves execute in the browser using unmodified native Web APIs.
        with (folder / 'app.js').open('a', encoding='utf-8') as stream:
            stream.write("\nwindow.runtimeCheck.then(report => reaper.ShowConsoleMsg('RUNTIME_REPORT:' + JSON.stringify(report) + '\\n'));\n")
    user32.PeekMessageW.argtypes = [C.POINTER(W.MSG), W.HWND, W.UINT, W.UINT, W.UINT]
    user32.TranslateMessage.argtypes = [C.POINTER(W.MSG)]
    user32.DispatchMessageW.argtypes = [C.POINTER(W.MSG)]

    def pump():
        msg = W.MSG()
        for _ in range(100):
            if not user32.PeekMessageW(C.byref(msg), None, 0, 0, 1): break
            user32.TranslateMessage(C.byref(msg)); user32.DispatchMessageW(C.byref(msg))
        host['timer']()
        time.sleep(.01)

    origins, reports = {}, []
    saved = root / 'web-runtime-report.json'
    prior = json.loads(saved.read_text(encoding='utf-8')) if saved.exists() else None
    first_a = prior[-1]['storageVisits'] + 1 if prior else 1
    first_b = prior[1]['storageVisits'] + 1 if prior else 1
    first_cookie = prior[-1]['cookieVisits'] + 1 if prior else 1
    active = {}
    if prior: origins = {app_a: prior[-1]['origin'], app_b: prior[1]['origin']}
    for index, (folder, visit) in enumerate(((app_a, first_a), (app_b, first_b), (app_a, first_a + 1))):
        if folder in active:
            assert host['close_window'](active.pop(folder))
            settle = time.monotonic() + .5
            while time.monotonic() < settle: pump()
        host['messages'].clear()
        identifier = host['open_window'](str(folder / 'index.html').encode('utf-8'))
        assert identifier, host['get_error']()
        active[folder] = identifier
        deadline = time.monotonic() + 35
        while time.monotonic() < deadline and not any(m.startswith('RUNTIME_REPORT:') for m in host['messages']): pump()
        captured = [m for m in host['messages'] if m.startswith('RUNTIME_REPORT:')]
        assert captured, host['messages']
        report = json.loads(captured[-1].split(':', 1)[1])
        assert report['passed'], report
        for name in ('storageVisits', 'indexedDBVisits'):
            assert report[name] == visit, (name, visit, report)
        assert report['cookieVisits'] == first_cookie + index, report
        assert all(check['ok'] for check in report['checks']), report
        assert report['runtime']['mode'] == 'app-http' and report['runtime']['storageIsolation'] == 'origin'
        if folder in origins: assert report['origin'] == origins[folder], 'Origin changed on reopen'
        origins[folder] = report['origin']
        reports.append(report)
        assert (root / 'ReaWebAPI/WebViewData').is_dir()
        assert not list((root / 'ReaWebAPI/Apps').glob('*/WebViewData'))
    for identifier in active.values(): assert host['close_window'](identifier)
    settle = time.monotonic() + .5
    while time.monotonic() < settle: pump()
    assert origins[app_a] != origins[app_b], origins
    (root / 'web-runtime-report.json').write_text(json.dumps(reports, ensure_ascii=False, indent=2), encoding='utf-8')
    print('WebView2: required Web Runtime checks, IndexedDB, both Worker kinds, WebGL, '
          'concurrent Apps, stable origin, reopen persistence, origin-isolated localStorage/IndexedDB and shared cookies passed')
