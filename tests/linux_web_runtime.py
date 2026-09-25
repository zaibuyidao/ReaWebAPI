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
data = fixture / 'ReaWebAPI' / 'WebViewData'
data.mkdir(parents=True)
try:
    for visits in (((0, 1), (1, 1), (0, 2)), ((0, 3),)):
        servers = []
        for app in apps:
            profile = fixture / 'ReaWebAPI' / 'Apps' / app.name
            server = subprocess.Popen([str(build / 'tests/web_resources_driver'), str(app), str(profile)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            servers.append(server)
            origin = server.stdout.readline().strip()
            assert origin.startswith('http://127.0.0.1:'), origin
            if app in origins: assert origins[app] == origin, 'Origin changed on restart'
            origins[app] = origin
        parent, child = socket.socketpair()
        helper = str(build / 'reawebapi-webview-x86_64')
        process = os.posix_spawn(helper, [helper, str(data)], dict(os.environ, GDK_BACKEND='x11'),
            file_actions=[(os.POSIX_SPAWN_DUP2, child.fileno(), 3), (os.POSIX_SPAWN_CLOSE, child.fileno())])
        child.close(); parent.setblocking(False)
        def send(message): parent.sendall((json.dumps(message) + '\n').encode())
        received, active = b'', {}
        try:
            for page_id, (app_index, visit) in enumerate(visits, 1):
                app = apps[app_index]
                if app in active: send(dict(id=active.pop(app), op='close'))
                active[app] = page_id
                send(dict(id=page_id, op='open', uri=origins[app] + '/index.html', script=bridge))
                send(dict(id=page_id, op='geometry', parent=window, x=app_index * 400, y=0,
                          width=400, height=800, visible=True))
                report = None
                deadline = time.monotonic() + 40
                while report is None and time.monotonic() < deadline:
                    if not select.select([parent], [], [], .05)[0]: continue
                    chunk = parent.recv(65536); assert chunk, 'WebKit exited early'
                    received += chunk
                    while b'\n' in received:
                        line, received = received.split(b'\n', 1); message = json.loads(line)
                        assert message['op'] != 'error', message
                        if message['op'] != 'message': continue
                        request = json.loads(message['message']); method = request['method']
                        value = None
                        if method == '__reawebHello': value = dict(protocol=1, projectEpoch=1, methods=methods)
                        elif method == 'GetAppVersion': value = '7.smoke'
                        elif method in ('ReaWeb_DocumentTitle', 'ReaWeb_Favicon'): value = True
                        elif method == 'ReaWeb_GetCapabilities':
                            value = dict(webRuntime=dict(contract=1, origin=origins[app], mode='app-http', storageIsolation='origin'))
                        elif method == 'ShowConsoleMsg':
                            assert message['id'] == page_id, message
                            report = json.loads(request['args'][0].split(':', 1)[1])
                        else: raise AssertionError(request)
                        response = dict(id=request['id'], document=request['document'], result=value)
                        send(dict(id=message['id'], op='eval', script='window.__reawebReceive(' + json.dumps(response) + ');'))
                assert report is not None, 'Timed out waiting for module App'
                assert report['passed'], report
                for name in ('storageVisits', 'indexedDBVisits'): assert report[name] == visit, (name, report)
                assert report['cookieVisits'] == len(reports) + 1, report
                # GPU access is optional under WSLg/headless CI.
                assert all(check['ok'] for check in report['checks'] if check['name'] != 'WebGL'), report
                assert report['origin'] == origins[app], report
                reports.append(report)
                print(f"App {app_index}: storage={visit}, cookies={report['cookieVisits']}")
            for page_id in active.values(): send(dict(id=page_id, op='close'))
            time.sleep(.3)
        finally:
            parent.close()
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if os.waitpid(process, os.WNOHANG)[0]: break
                time.sleep(.02)
            else: os.kill(process, signal.SIGKILL); os.waitpid(process, 0)
            for server in servers: server.communicate('\n', timeout=8)
    assert origins[apps[0]] != origins[apps[1]]
    assert not list((fixture / 'ReaWebAPI' / 'Apps').glob('*/WebViewData'))
    (fixture / 'report.json').write_text(json.dumps(reports, indent=2), encoding='utf-8')
    print('WebKitGTK: concurrent Apps, restart persistence, origin-isolated localStorage/IndexedDB, shared cookies and both Workers passed')
finally:
    network.shutdown(); network.server_close()
    x.XDestroyWindow(display, window); x.XCloseDisplay(display)
