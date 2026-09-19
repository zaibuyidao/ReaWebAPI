"""Exercise the actual native resource origin over HTTP, without REAPER."""
import concurrent.futures
import http.client
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from urllib.parse import quote, urlsplit

BINARY = sys.argv.pop(1)


class ResourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.root = self.base / 'App'
        self.root.mkdir()
        self.profile = self.base / 'profile'
        (self.root / 'index.html').write_text('<script type="module" src="./app.js"></script>', encoding='utf-8')
        (self.root / 'app.js').write_text('export const value = 42;', encoding='utf-8')
        (self.root / 'data.json').write_text('{"value":42}', encoding='utf-8')
        (self.root / 'file # % 中文.mjs').write_text('export default "中文";', encoding='utf-8')
        (self.root / 'style.css').write_text('body { color: red; }', encoding='utf-8')
        (self.root / 'module.wasm').write_bytes(b'\0asm')
        (self.root / 'binary.bin').write_bytes(bytes(range(256)) * 4096)
        (self.base / 'secret.txt').write_text('outside')
        self.start()

    def start(self):
        self.process = subprocess.Popen([BINARY, str(self.root), str(self.profile)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding='utf-8')
        self.origin = self.process.stdout.readline().strip()
        hostname = 'localhost' if sys.platform == 'darwin' else '127.0.0.1'
        self.assertTrue(self.origin.startswith('http://' + hostname + ':'), self.origin)

    def stop(self):
        self.process.communicate('\n', timeout=8)
        self.assertEqual(self.process.returncode, 0)

    def tearDown(self):
        if self.process.poll() is None:
            self.stop()
        self.temp.cleanup()

    def request(self, path, method='GET', headers=None):
        url = urlsplit(self.origin)
        client = http.client.HTTPConnection(url.hostname, url.port, timeout=4)
        try:
            client.request(method, path, headers=headers or {})
            response = client.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            client.close()

    def test_mime_unicode_query_range_and_head(self):
        for path, mime in [('app.js', 'text/javascript'), ('data.json', 'application/json'),
                           ('style.css', 'text/css'), ('module.wasm', 'application/wasm'),
                           ('file # % 中文.mjs', 'text/javascript')]:
            status, headers, body = self.request('/' + quote(path) + '?v=1')
            self.assertEqual(status, 200)
            self.assertTrue(headers['Content-Type'].startswith(mime))
            self.assertEqual(body, (self.root / path).read_bytes())
            self.assertEqual(headers['X-Content-Type-Options'], 'nosniff')
        status, headers, body = self.request('/binary.bin', headers={'Range': 'bytes=100-199'})
        self.assertEqual((status, body), (206, bytes(range(100, 200))))
        self.assertEqual(headers['Content-Range'], 'bytes 100-199/1048576')
        status, headers, body = self.request('/binary.bin', 'HEAD')
        self.assertEqual((status, body, int(headers['Content-Length'])), (200, b'', 1048576))
        self.assertEqual(self.request('/missing.js')[0], 404)

    def test_origin_and_read_only_boundary(self):
        self.assertEqual(self.request('/data.json', headers={'Origin': self.origin})[0], 200)
        self.assertEqual(self.request('/data.json', headers={'Origin': 'https://example.com'})[0], 403)
        self.assertEqual(self.request('/data.json', headers={'Host': 'attacker.example'})[0], 403)
        self.assertEqual(self.request('/data.json', headers={'Sec-Fetch-Site': 'cross-site'})[0], 403)
        self.assertEqual(self.request('/data.json', 'POST')[0], 405)
        for path in ['/../secret.txt', '/%2e%2e/secret.txt', '/..%5csecret.txt', '/C:/secret.txt', '/a%00.js']:
            self.assertNotEqual(self.request(path)[0], 200, path)
        self.assertNotIn('Access-Control-Allow-Origin', self.request('/data.json')[1])
        try:
            (self.root / 'escape.txt').symlink_to(self.base / 'secret.txt')
        except OSError:
            return
        self.assertEqual(self.request('/escape.txt')[0], 403)

    def test_persisted_origin_and_port_conflict(self):
        initial = self.origin
        duplicate = subprocess.run([BINARY, str(self.root), str(self.profile)], input='\n',
                                   capture_output=True, text=True, encoding='utf-8', timeout=8)
        self.assertEqual(duplicate.returncode, 2)
        self.assertIn('APP_ORIGIN_BUSY', duplicate.stdout)
        self.stop()
        self.start()
        self.assertEqual(self.origin, initial)
        self.assertEqual(json.loads((self.profile / 'origin.json').read_text())['schema'], 1)
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            for status, _, body in pool.map(lambda _: self.request('/binary.bin'), range(12)):
                self.assertEqual(status, 200)
                self.assertEqual(len(body), 1048576)


if __name__ == '__main__':
    unittest.main()
