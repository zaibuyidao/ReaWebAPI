"""Local CORS and WebSocket fixtures for actual browser tests; no dependencies."""
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_): pass

    def do_GET(self):
        if self.headers.get('Upgrade', '').lower() == 'websocket':
            key = self.headers['Sec-WebSocket-Key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
            self.send_response(101)
            self.send_header('Upgrade', 'websocket'); self.send_header('Connection', 'Upgrade')
            self.send_header('Sec-WebSocket-Accept', base64.b64encode(hashlib.sha1(key.encode()).digest()).decode())
            self.end_headers()
            # Only the short, masked text frame sent by the fixture is expected.
            header = self.rfile.read(2)
            if len(header) != 2 or header[0] != 0x81 or header[1] != 0x84: return
            mask, payload = self.rfile.read(4), self.rfile.read(4)
            if len(mask) != 4 or len(payload) != 4: return
            decoded = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
            self.wfile.write(b'\x81\x04' + decoded); self.wfile.flush()
            self.close_connection = True
        else:
            body = json.dumps(dict(value=42)).encode()
            self.send_response(200)
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close'); self.end_headers(); self.wfile.write(body)


def start():
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    port = server.server_address[1]
    return server, dict(http=f'http://127.0.0.1:{port}/data', websocket=f'ws://127.0.0.1:{port}/echo')


def configure(folder, endpoints):
    (folder / 'endpoints.js').write_text('window.reawebProbeEndpoints = ' + json.dumps(endpoints) + ';', encoding='utf-8')
    page = folder / 'index.html'
    page.write_text(page.read_text(encoding='utf-8').replace('<script type="module"', '<script src="./endpoints.js"></script><script type="module"'), encoding='utf-8')
