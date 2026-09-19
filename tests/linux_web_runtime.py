"""Run the distributed ES module conformance App in real WebKitGTK/X11."""
import ctypes as C
import json
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import subprocess
import time
import uuid
import web_runtime_network

root = Path(__file__).resolve().parents[1]
build = root / os.environ.get('REAWEB_TEST_BUILD_DIR', 'build-linux')
fixture = build / ('web-runtime-' + uuid.uuid4().hex)
fixture.mkdir()
apps = [fixture / name for name in ('空 格#% AppA', 'AppB')]
network, endpoints = web_runtime_network.start()
for app in apps:
    shutil.copytree(root / 'runtime/web-runtime', app)
    web_runtime_network.configure(app, endpoints)
    with (app / 'app.js').open('a', encoding='utf-8') as stream:
        stream.write("\nwindow.runtimeCheck.then(report => reaper.ShowConsoleMsg('RUNTIME_REPORT:' + JSON.stringify(report)));\n")
x = C.CDLL('libX11.so.6')
x.XOpenDisplay.argtypes = [C.c_char_p]; x.XOpenDisplay.restype = C.c_void_p
x.XDefaultRootWindow.argtypes = [C.c_void_p]; x.XDefaultRootWindow.restype = C.c_ulong
x.XCreateSimpleWindow.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_uint, C.c_ulong, C.c_ulong]
x.XCreateSimpleWindow.restype = C.c_ulong
x.XMapWindow.argtypes = [C.c_void_p, C.c_ulong]; x.XDestroyWindow.argtypes = [C.c_void_p, C.c_ulong]
x.XSync.argtypes = [C.c_void_p, C.c_int]; x.XCloseDisplay.argtypes = [C.c_void_p]
display = x.XOpenDisplay(None); assert display, 'X11 display required'
window = x.XCreateSimpleWindow(display, x.XDefaultRootWindow(display), 30, 30, 800, 800, 0, 0, 0)
x.XMapWindow(display, window); x.XSync(display, 0)
bridge = '(() => {\n' + (root / 'runtime/reaper-api.generated.js').read_text() + (root / 'runtime/reaper.js').read_text() + '\n})();'
methods = list(json.loads((root / 'api/bindings.json').read_text())['functions'])
origins, reports = {}, []
try:
    for app, visit in ((apps[0], 1), (apps[1], 1), (apps[0], 2)):
        profile = fixture / (app.name + '-profile')
        server = subprocess.Popen([str(build / 'tests/web_resources_driver'), str(app), str(profile)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        origin = server.stdout.readline().strip(); assert origin.startswith('http://127.0.0.1:'), origin
        data = profile / 'WebViewData'; data.mkdir(exist_ok=True)
        parent, child = socket.socketpair()
        helper = str(build / 'reawebapi-webview-x86_64')
        process = os.posix_spawn(helper, [helper, str(data)], dict(os.environ, GDK_BACKEND='x11'),
            file_actions=[(os.POSIX_SPAWN_DUP2, child.fileno(), 3), (os.POSIX_SPAWN_CLOSE, child.fileno())])
        child.close(); parent.setblocking(False)
        def send(message): parent.sendall((json.dumps(message) + '\n').encode())
        received, report = b'', None
        try:
            deadline = time.monotonic() + 40
            while report is None and time.monotonic() < deadline:
                if not select.select([parent], [], [], .05)[0]: continue
                chunk = parent.recv(65536); assert chunk, 'WebKit exited early'
                received += chunk
                while b'\n' in received:
                    line, received = received.split(b'\n', 1); message = json.loads(line)
                    assert message['op'] != 'error', message
                    if message['op'] == 'ready':
                        send(dict(id=1, op='open', uri=origin + '/index.html', script=bridge))
                        send(dict(id=1, op='geometry', parent=window, x=0, y=0, width=800, height=800, visible=True))
                    elif message['op'] == 'message':
                        request = json.loads(message['message']); method = request['method']
                        value = None
                        if method == '__reawebHello': value = dict(protocol=1, projectEpoch=1, methods=methods)
                        elif method == 'GetAppVersion': value = '7.smoke'
                        elif method == 'ReaWeb_GetCapabilities': value = dict(webRuntime=dict(contract=1, origin=origin, mode='app-http', storageIsolation='app-profile'))
                        elif method == 'ShowConsoleMsg': report = json.loads(request['args'][0].split(':', 1)[1])
                        else: raise AssertionError(request)
                        response = dict(id=request['id'], document=request['document'], result=value)
                        send(dict(id=1, op='eval', script='window.__reawebReceive(' + json.dumps(response) + ');'))
            assert report is not None, 'Timed out waiting for module App'
            print(json.dumps(report, ensure_ascii=False))
            assert report['passed'], report
            for name in ('storageVisits', 'cookieVisits', 'indexedDBVisits'): assert report[name] == visit, (name, report)
            # GPU access is optional under WSLg/headless CI; all other exercised checks must pass.
            assert all(check['ok'] for check in report['checks'] if check['name'] != 'WebGL'), report
            if app in origins: assert origins[app] == origin, 'Origin changed across browser process restart'
            origins[app] = origin; reports.append(report)
            send(dict(id=1, op='close'))
            time.sleep(.3)
        finally:
            parent.close()
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if os.waitpid(process, os.WNOHANG)[0]: break
                time.sleep(.02)
            else: os.kill(process, signal.SIGKILL); os.waitpid(process, 0)
            server.communicate('\n', timeout=8)
    assert origins[apps[0]] != origins[apps[1]]
    (fixture / 'report.json').write_text(json.dumps(reports, indent=2), encoding='utf-8')
    print('WebKitGTK: required Web Runtime checks, restart persistence, cross-App storage/cookie/IndexedDB isolation and both Workers passed')
finally:
    network.shutdown(); network.server_close()
    x.XDestroyWindow(display, window); x.XCloseDisplay(display)
