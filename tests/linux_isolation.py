"""Probe production custom-origin isolation in real WebKitGTK without changing engine flags."""
import ctypes as C
import json
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread

source = Path(__file__).resolve().parents[1]
build, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
output.mkdir(parents=True, exist_ok=False)
shutil.copyfile(source / 'tests/webkit_isolation_probe.cjs', output / 'probe.js')
(output / 'index.html').write_text("<!doctype html><script>webkit.messageHandlers.reaweb.postMessage('APP_EXECUTED')</script><script src='probe.js'></script>")
x = C.CDLL('libX11.so.6')
x.XOpenDisplay.argtypes = [C.c_char_p]; x.XOpenDisplay.restype = C.c_void_p
x.XDefaultRootWindow.argtypes = [C.c_void_p]; x.XDefaultRootWindow.restype = C.c_ulong
x.XCreateSimpleWindow.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_uint, C.c_ulong, C.c_ulong]
x.XCreateSimpleWindow.restype = C.c_ulong
for name in ['XMapWindow', 'XDestroyWindow']:
    getattr(x, name).argtypes = [C.c_void_p, C.c_ulong]
x.XSync.argtypes = [C.c_void_p, C.c_int]; x.XCloseDisplay.argtypes = [C.c_void_p]
display = x.XOpenDisplay(None)
assert display, 'An X11/WSLg display is required'
window = x.XCreateSimpleWindow(display, x.XDefaultRootWindow(display), 20, 20, 640, 480, 0, 0, 0)
x.XMapWindow(display, window); x.XSync(display, 0)
parent, child = socket.socketpair()
helper = str(build / 'reawebapi-webview-x86_64')
pid = os.posix_spawn(helper, [helper, str(output / 'profile')], dict(os.environ, GDK_BACKEND='x11'),
    file_actions=[(os.POSIX_SPAWN_DUP2, child.fileno(), 3), (os.POSIX_SPAWN_CLOSE, child.fileno())])
child.close(); parent.setblocking(False)
buffer, reports = b'', []
bridge = '(() => {const reawebIsolationRequired=true;\n' + (source / 'runtime/reaper-api.generated.js').read_text() + (source / 'runtime/reaper.js').read_text() + '\n})();'
worker_source = (source / 'src/web/browser_probe.hpp').read_text().split('R"JS(', 1)[1].split(')JS";', 1)[0].encode()
class ControlHandler(BaseHTTPRequestHandler):
    required = False
    def do_GET(self):
        if self.path == '/__reaweb_runtime__/probe.js': data, mime = worker_source, 'text/javascript'
        elif self.path == '/probe.js': data, mime = (output / 'probe.js').read_bytes(), 'text/javascript'
        elif self.path == '/index.html': data, mime = (output / 'index.html').read_bytes(), 'text/html'
        else: self.send_error(404); return
        self.send_response(200)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
        if self.required:
            self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
            self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
            self.send_header('Permissions-Policy', 'cross-origin-isolated=(self)')
        self.end_headers(); self.wfile.write(data)
    def log_message(self, *args): pass
server = ThreadingHTTPServer(('127.0.0.1', 0), ControlHandler)
listener = Thread(target=server.serve_forever, daemon=True); listener.start()

def send(value): parent.sendall((json.dumps(value) + '\n').encode())

try:
    for phase in range(6):
        page_id = phase + 1
        row = {'phase': phase, 'messages': [], 'nativeEvents': []}
        ControlHandler.required = phase % 3 != 0
        uri = 'reaweb://webkit-isolation-acceptance/index.html' if phase < 3 else f'http://127.0.0.1:{server.server_port}/index.html'
        send({'op': 'open', 'id': page_id, 'uri': uri,
              'resourceRoot': str(output) if phase < 3 else '', 'isolationRequired': phase % 3 != 0, 'script': bridge if phase % 3 == 2 else ''})
        send({'op': 'geometry', 'id': page_id, 'parent': window, 'x': 0, 'y': 0, 'width': 640, 'height': 480, 'visible': True})
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if not select.select([parent], [], [], .05)[0]: continue
            chunk = parent.recv(65536)
            assert chunk, 'WebKit helper exited before reporting'
            buffer += chunk
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                event = json.loads(line)
                if event.get('id') != page_id: continue
                if event['op'] == 'message': row['messages'].append(event['message'])
                elif event['op'] in ('error', 'loaded'): row['nativeEvents'].append(event)
        reports.append(row)
        send({'op': 'close', 'id': page_id})
        (output / 'report.json').write_text(json.dumps(reports, indent=2))
    print(json.dumps(reports, indent=2))
finally:
    server.shutdown(); server.server_close(); listener.join()
    parent.close()
    for _ in range(100):
        if os.waitpid(pid, os.WNOHANG)[0]: break
        time.sleep(.05)
    else:
        os.kill(pid, signal.SIGTERM); os.waitpid(pid, 0)
    x.XDestroyWindow(display, window); x.XCloseDisplay(display)
