"""Exercise native virtual resources and persistent App identity without a listener."""
import base64
import json
import os
import shutil
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
        (self.root / 'app.json').write_text('{"id":"timefold"}')
        self.start()

    def start(self):
        self.process = subprocess.Popen([BINARY, str(self.root), str(self.profile)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding='utf-8')
        self.origin = self.process.stdout.readline().strip()
        self.assertEqual(self.origin, 'reaweb://timefold')

    def stop(self):
        self.process.communicate('\n', timeout=8)
        self.assertEqual(self.process.returncode, 0)

    def tearDown(self):
        if self.process.poll() is None:
            self.stop()
        # Remove only junction entries, never recurse through their targets.
        for junction in getattr(self, 'junctions', []):
            junction.rmdir()
        self.temp.cleanup()

    def request(self, path, method='GET', headers=None):
        request = dict(uri=self.origin + path, method=method, headers={k.lower(): v for k,v in (headers or {}).items()})
        self.process.stdin.write(json.dumps(request) + '\n'); self.process.stdin.flush()
        response = json.loads(self.process.stdout.readline())
        return response['status'], response['headers'], base64.b64decode(response['body']['__reawebBytes'])

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

    def test_devtools_settings_fallback_and_app_override(self):
        path = '/.well-known/appspecific/com.chrome.devtools.json'
        for suffix in ('', '?check=1'):
            status, headers, body = self.request(path + suffix)
            self.assertEqual((status, json.loads(body)), (200, {}))
            self.assertTrue(headers['Content-Type'].startswith('application/json'))
            self.assertEqual(headers['X-Content-Type-Options'], 'nosniff')
            self.assertNotIn('Access-Control-Allow-Origin', headers)
        status, headers, body = self.request(path, 'HEAD')
        self.assertEqual((status, body, int(headers['Content-Length'])), (200, b'', 2))
        self.assertEqual(self.request(path, 'POST')[0], 405)
        self.assertEqual(self.request(path, headers={'Origin': 'https://example.com'})[0], 403)
        self.assertEqual(self.request(path, headers={'Host': 'attacker.example'})[0], 403)
        self.assertEqual(self.request(path, headers={'Sec-Fetch-Site': 'cross-site'})[0], 403)
        self.assertEqual(self.request(path + '.missing')[0], 404)
        self.assertEqual(self.request('/missing.json')[0], 404)
        custom = self.root / path.lstrip('/')
        custom.parent.mkdir(parents=True)
        config = {'workspace': {'root': str(self.root), 'uuid': 'c0dd23bf-0354-481b-9b52-4afe463fc534'}}
        custom.write_text(json.dumps(config), encoding='utf-8')
        status, _, body = self.request(path)
        self.assertEqual((status, json.loads(body)), (200, config))

    def symlink(self, link, target, directory=False):
        try:
            link.symlink_to(target, target_is_directory=directory)
        except OSError as error:
            if sys.platform == 'win32' and error.winerror == 1314:
                self.skipTest('Windows symlink privilege unavailable; junction coverage still runs')
            raise

    def assert_forbidden(self, path):
        for method, headers in [('GET', {}), ('HEAD', {}), ('GET', {'Range': 'bytes=0-3'})]:
            with self.subTest(path=path, method=method, headers=headers):
                status, _, body = self.request(path, method, headers)
                self.assertEqual(status, 403)
                self.assertNotIn(b'outside', body)

    def test_file_symlink_escape(self):
        self.symlink(self.root / 'escape.txt', self.base / 'secret.txt')
        self.assert_forbidden('/escape.txt')

    def test_directory_symlink_escape(self):
        self.symlink(self.root / 'escape', self.base, directory=True)
        self.assert_forbidden('/escape/secret.txt')

    def test_index_symlink_escape(self):
        directory = self.root / 'pages'
        directory.mkdir()
        self.symlink(directory / 'index.html', self.base / 'secret.txt')
        self.assert_forbidden('/pages/')

    def test_internal_symlink_resources(self):
        self.symlink(self.root / 'alias.json', self.root / 'data.json')
        self.assertEqual(self.request('/alias.json')[2], (self.root / 'data.json').read_bytes())

    @unittest.skipUnless(sys.platform == 'win32', 'Windows junctions only')
    def test_windows_junction_boundary(self):
        outside = self.base / 'App-other'
        outside.mkdir()
        (outside / 'secret.txt').write_text('outside')
        (outside / 'index.html').write_text('outside')
        internal = self.root / 'assets'
        internal.mkdir()
        (internal / 'data.json').write_text('{"value":42}')
        self.junctions = []
        for name, target in [('escape-dir', outside), ('inside-dir', internal)]:
            link = self.root / name
            subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
                            "$ErrorActionPreference = 'Stop'; New-Item -ItemType Junction "
                            '-Path $env:REAWEB_TEST_LINK -Target $env:REAWEB_TEST_TARGET | Out-Null'],
                           env={**os.environ, 'REAWEB_TEST_LINK': str(link), 'REAWEB_TEST_TARGET': str(target)},
                           check=True, capture_output=True, timeout=15)
            self.junctions.append(link)
        self.assert_forbidden('/escape-dir/secret.txt')
        self.assert_forbidden('/escape-dir/')
        self.assertEqual(self.request('/inside-dir/data.json')[2], b'{"value":42}')

    def test_persisted_origin_without_port(self):
        initial = self.origin
        duplicate = subprocess.run([BINARY, str(self.root), str(self.profile)], input='\n',
                                   capture_output=True, text=True, encoding='utf-8', timeout=8)
        self.assertEqual(duplicate.returncode, 0, duplicate.stdout)
        self.assertEqual(duplicate.stdout.strip(), self.origin)
        self.stop()
        self.start()
        self.assertEqual(self.origin, initial)
        record = json.loads((self.profile / 'origin.json').read_text())
        self.assertEqual(record['schema'], 2)
        self.assertEqual(record['entry'], 'index.html')
        self.assertNotIn('port', record)
        self.assertEqual(record['appId'], 'timefold')
        status, _, body = self.request('/binary.bin')
        self.assertEqual((status, len(body)), (200, 1048576))

    def launch(self, root=None, profile=None, source='@zaibuyidao_ReaGBA.lua', entry='index.html'):
        return subprocess.run([BINARY, str(root or self.root), str(profile or self.profile), source, entry],
            input='\n', capture_output=True, text=True, encoding='utf-8', timeout=8)

    def record(self):
        return json.loads((self.profile / 'origin.json').read_text(encoding='utf-8'))

    def write_record(self, value):
        (self.profile / 'origin.json').write_text(json.dumps(value), encoding='utf-8')

    def copy_root(self, name='Copy'):
        other = self.base / name
        shutil.copytree(self.root, other)
        return other

    def assert_rebound(self, other, entry='index.html'):
        before = self.record()
        before['retainedExtension'] = {'value': 17}
        self.write_record(before)
        retained = [self.profile / 'Data' / 'setting', self.profile / 'WindowState' / 'state',
                    self.profile / 'WebViewData' / 'legacy', self.base / 'WebViewData' / 'browser']
        for path in retained:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'unchanged')
        result = self.launch(root=other, entry=entry)
        self.assertEqual(result.returncode, 0, result.stdout)
        after = self.record()
        self.assertEqual(after['entry'], entry)
        self.assertTrue(Path(after['root']).samefile(other))
        for key in before.keys() - {'root', 'entry'}:
            self.assertEqual(after[key], before[key])
        for path in retained:
            self.assertEqual(path.read_bytes(), b'unchanged')
        self.assertEqual(self.launch(root=other, entry=entry).returncode, 0)

    def test_recover_missing_root_with_and_without_entry(self):
        self.stop()
        original = self.record()
        moved = self.base / 'Moved'
        self.root.rename(moved)
        for legacy in (False, True):
            data = dict(original)
            if legacy:
                data.pop('entry')
            self.write_record(data)
            self.assert_rebound(moved)

    def test_recover_recorded_entry_only(self):
        self.stop()
        other = self.copy_root()
        (self.root / 'other.html').write_text('another document')
        for invalid in ('missing', 'directory'):
            with self.subTest(invalid=invalid):
                self.write_record({**self.record(), 'root': self.root.as_posix(), 'entry': 'index.html'})
                page = self.root / 'index.html'
                if page.is_file():
                    page.unlink()
                if invalid == 'directory':
                    page.mkdir()
                self.assert_rebound(other)

    def test_deleted_root_without_entry_metadata(self):
        self.stop()
        other = self.copy_root()
        data = self.record()
        data.pop('entry')
        self.write_record(data)
        for child in self.root.iterdir():
            child.unlink()
        self.root.rmdir()
        self.assert_rebound(other)

    def test_recorded_entry_parent_is_no_longer_a_directory(self):
        self.stop()
        other = self.copy_root()
        self.write_record({**self.record(), 'entry': 'pages/start.html'})
        (self.root / 'pages').write_text('directory replaced by a file')
        self.assert_rebound(other)

    def test_non_index_entry_and_same_root_preserves_original_entry(self):
        self.stop()
        data = self.record()
        data.pop('entry')
        self.write_record(data)
        (self.root / 'launch.HTM').write_text('original entry')
        self.assertEqual(self.launch(entry='launch.HTM').returncode, 0)
        self.assertEqual(self.record()['entry'], 'launch.HTM')
        self.assertEqual(self.launch().returncode, 0)
        self.assertEqual(self.record()['entry'], 'launch.HTM')
        other = self.copy_root()
        (self.root / 'launch.HTM').unlink()
        self.assert_rebound(other, 'launch.HTM')

    def test_legacy_existing_root_cannot_guess_entry(self):
        self.stop()
        other = self.copy_root()
        data = self.record()
        data.pop('entry')
        self.write_record(data)
        (self.root / 'index.html').unlink()
        result = self.launch(root=other)
        self.assertIn('APP_ID_CONFLICT', result.stdout)
        self.assertIn('Missing entry metadata', result.stdout)
        self.assertEqual(self.record(), data)
        (self.root / 'index.html').write_text('restored')
        self.assertEqual(self.launch().returncode, 0)
        self.assertEqual(self.record()['entry'], 'index.html')

    def test_legacy_manifest_entry_is_authoritative(self):
        self.stop()
        other = self.copy_root()
        data = self.record()
        data.pop('entry')
        (self.root / 'app.json').write_text('{"id":"timefold","entry":"original.htm"}')
        (self.root / 'original.htm').write_text('original')
        self.write_record(data)
        self.assertIn('original entry is valid', self.launch(root=other).stdout)
        (self.root / 'original.htm').unlink()
        self.assert_rebound(other)

    def test_legacy_manifest_errors_do_not_allow_takeover(self):
        self.stop()
        other = self.copy_root()
        data = self.record()
        data.pop('entry')
        self.write_record(data)
        for manifest in ('{broken', '{"id":"other","entry":"missing.html"}',
                         '{"entry":"../outside.html"}', '{"schemaVersion":2,"entry":"missing.html"}'):
            (self.root / 'app.json').write_text(manifest)
            self.assertIn('APP_ID_CONFLICT', self.launch(root=other).stdout)
            self.assertEqual(self.record(), data)

    def test_active_binding_blocks_recovery_across_processes(self):
        other = self.copy_root()
        before = self.record()
        (self.root / 'index.html').unlink()
        result = self.launch(root=other)
        self.assertIn('active or initializing windows', result.stdout)
        self.assertEqual(self.record(), before)
        moved = self.base / 'RemovedRoot'
        self.root.rename(moved)
        result = self.launch(root=other)
        self.assertIn('active or initializing windows', result.stdout)
        self.assertEqual(self.record(), before)
        self.stop()
        self.assert_rebound(other)

    def test_conflict_diagnostics(self):
        other = self.copy_root()
        result = self.launch(root=other)
        for text in ('APP_ID_CONFLICT', 'timefold', 'old Root exists', 'original entry is valid',
                     'origin.json', 'remove its directory and retry'):
            self.assertIn(text, result.stdout)
        self.assertIn(other.as_posix().lower(), result.stdout.replace('\\', '/').lower())

    def test_invalid_new_entry_does_not_change_binding(self):
        self.stop()
        other = self.copy_root()
        (self.root / 'index.html').unlink()
        before = (self.profile / 'origin.json').read_bytes()
        for entry in ('missing.html', 'app.js', '../secret.txt'):
            result = self.launch(root=other, entry=entry)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((self.profile / 'origin.json').read_bytes(), before)

    def test_invalid_records_are_never_overwritten(self):
        self.stop()
        original = self.record()
        other = self.copy_root()
        (self.root / 'index.html').unlink()
        for update in ({'schema': 3}, {'schema': 2.5}, {'schema': 4294967298}, {'appId': 'wrong'}, {'origin': 'reaweb://wrong'},
                       {'root': 'relative'}, {'entry': '../outside.html'}, {'entry': '/outside.html'},
                       {'entry': 'C:/outside.html'}, {'entry': 'a\\b.html'}, {'entry': 12}, {'entry': 'app.js'}):
            with self.subTest(update=update):
                self.write_record({**original, **update})
                before = (self.profile / 'origin.json').read_bytes()
                self.assertIn('APP_ORIGIN_INVALID', self.launch(root=other).stdout)
                self.assertEqual((self.profile / 'origin.json').read_bytes(), before)
        (self.profile / 'origin.json').write_text('{broken')
        self.assertIn('APP_ORIGIN_INVALID', self.launch(root=other).stdout)
        self.assertEqual((self.profile / 'origin.json').read_text(), '{broken')

    def test_same_root_normalization(self):
        self.stop()
        data = self.record()
        (self.root / 'child').mkdir()
        data['root'] = str(self.root / 'child' / '..' / '.')
        if sys.platform == 'win32':
            data['root'] = data['root'].upper().replace('/', '\\')
        self.write_record(data)
        self.assertEqual(self.launch().returncode, 0)

    @unittest.skipIf(sys.platform == 'win32', 'Case-sensitive filesystem required')
    def test_case_distinct_roots_conflict(self):
        other = self.base / 'app'
        if other.exists():
            self.skipTest('Filesystem is case insensitive')
        shutil.copytree(self.root, other)
        self.assertIn('APP_ID_CONFLICT', self.launch(root=other).stdout)

    def test_concurrent_recovery_has_one_winner(self):
        self.stop()
        roots = [self.copy_root('Copy1'), self.copy_root('Copy2')]
        (self.root / 'index.html').unlink()
        processes = [subprocess.Popen([BINARY, str(root), str(self.profile)], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, text=True, encoding='utf-8') for root in roots]
        try:
            lines = [process.stdout.readline().strip() for process in processes]
            self.assertEqual(sum(line == self.origin for line in lines), 1, lines)
            self.assertEqual(sum('APP_ID_CONFLICT' in line for line in lines), 1, lines)
            winner = lines.index(self.origin)
            self.assertTrue(Path(self.record()['root']).samefile(roots[winner]))
        finally:
            for process in processes:
                process.communicate('\n', timeout=8)

    def test_recorded_entry_cannot_escape_through_symlink(self):
        self.stop()
        other = self.copy_root()
        outside = self.base / 'outside.html'
        outside.write_text('outside')
        (self.root / 'index.html').unlink()
        self.symlink(self.root / 'index.html', outside)
        before = self.record()
        self.assertIn('APP_ORIGIN_INVALID', self.launch(root=other).stdout)
        self.assertEqual(self.record(), before)

    @unittest.skipUnless(sys.platform == 'win32', 'Windows junctions only')
    def test_binding_entry_junction_boundary(self):
        self.stop()
        other = self.copy_root()
        outside = self.base / 'Outside'
        outside.mkdir()
        (outside / 'page.html').write_text('outside')
        link = self.root / 'escape'
        subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
                        "$ErrorActionPreference = 'Stop'; New-Item -ItemType Junction "
                        '-Path $env:REAWEB_TEST_LINK -Target $env:REAWEB_TEST_TARGET | Out-Null'],
                       env={**os.environ, 'REAWEB_TEST_LINK': str(link), 'REAWEB_TEST_TARGET': str(outside)},
                       check=True, capture_output=True, timeout=15)
        self.junctions = [link]
        self.assertIn('INVALID_PATH', self.launch(entry='escape/page.html').stdout)
        data = {**self.record(), 'entry': 'escape/page.html'}
        self.write_record(data)
        for missing in (False, True):
            if missing:
                (outside / 'page.html').unlink()
            self.assertIn('APP_ORIGIN_INVALID', self.launch(root=other).stdout)
            self.assertEqual(self.record(), data)

    @unittest.skipUnless(sys.platform == 'win32', 'Windows drive status')
    def test_unavailable_drive_is_not_a_deleted_root(self):
        self.stop()
        drive = next((letter for letter in 'ZYXWVUTSRQPONMLKJIHGFE' if not Path(letter + ':/').exists()), None)
        if drive is None:
            self.skipTest('No unused drive letter')
        data = self.record()
        data['root'] = drive + ':/Unavailable/App'
        self.write_record(data)
        result = self.launch()
        self.assertIn('APP_ID_CONFLICT', result.stdout)
        self.assertIn('Cannot confirm', result.stdout)
        self.assertEqual(self.record(), data)

    @unittest.skipIf(sys.platform == 'win32', 'POSIX permissions')
    def test_unreadable_old_path_is_not_missing(self):
        if os.geteuid() == 0:
            self.skipTest('Root bypasses file permissions')
        self.stop()
        other = self.copy_root()
        before = self.record()
        self.root.chmod(0)
        try:
            result = self.launch(root=other)
            self.assertIn('APP_ID_CONFLICT', result.stdout)
            self.assertIn('Cannot confirm', result.stdout)
            self.assertEqual(self.record(), before)
        finally:
            self.root.chmod(0o700)

    def test_identity_conflict_and_move(self):
        other = self.base / 'Copy'
        shutil.copytree(self.root, other)
        self.assertIn('APP_ID_CONFLICT', self.launch(root=other).stdout)
        self.stop()
        self.assertIn('APP_ID_CONFLICT', self.launch(root=other).stdout)
        moved = self.base / 'Renamed App'
        self.root.rename(moved)
        self.root = moved
        self.start()
        self.assertEqual(self.origin, 'reaweb://timefold')
        self.assertEqual(self.request('/data.json')[0], 200)

    def test_manifest_precedence_and_launcher_fallback(self):
        self.assertEqual(self.launch(source='not-a-lua-source').stdout.strip(), 'reaweb://timefold')
        self.stop()
        (self.root / 'app.json').write_text('{"name":"Ignored display name"}')
        (self.root / 'Wrong.lua').write_text('-- Never infer identity from directory contents')
        fallback = self.base / 'fallback'
        for source in ('@/Scripts/zaibuyidao_ReaGBA.lua', r'C:\Scripts\Zaibuyidao ReaGBA.LUA'):
            self.assertEqual(self.launch(profile=fallback, source=source).stdout.strip(), 'reaweb://zaibuyidao-reagba')
        self.assertIn('APP_ID_REQUIRED', self.launch(profile=fallback, source='').stdout)
        copy = self.base / 'OtherRoot'
        shutil.copytree(self.root, copy)
        self.assertIn('APP_ID_CONFLICT', self.launch(root=copy, profile=fallback).stdout)

    def test_invalid_ids_are_not_silently_rewritten(self):
        self.stop()
        for identity in ('', 'TimeFold', 'a_b', 'with space', '../escape', 'a.b', '音', None, 42):
            (self.root / 'app.json').write_text(json.dumps({'id': identity}))
            self.assertIn('APP_MANIFEST_INVALID', self.launch(profile=self.base / 'invalid').stdout, identity)

    def test_old_metadata_upgrade_retains_browser_data(self):
        self.stop()
        record = self.profile / 'origin.json'
        record.write_text(json.dumps({'schema': 1, 'root': str(self.root.resolve()).replace('\\', '/'), 'port': 12345}))
        browser = self.profile / 'WebViewData'
        browser.mkdir(); (browser / 'retained').write_text('old browser state')
        self.start()
        data = json.loads(record.read_text())
        self.assertEqual((data['schema'], data['origin']), (2, 'reaweb://timefold'))
        self.assertNotIn('port', data)
        self.assertEqual((browser / 'retained').read_text(), 'old browser state')

    def test_ranges_cache_and_strict_paths(self):
        self.assertEqual(self.request('/binary.bin', headers={'Range': 'bytes=-2'})[2], bytes([254, 255]))
        self.assertEqual(self.request('/binary.bin', headers={'Range': 'bytes=1048576-'})[0], 416)
        self.assertEqual(self.request('/binary.bin', headers={'Range': 'bytes=2-1'})[0], 400)
        status, headers, body = self.request('/data.json')
        self.assertEqual(self.request('/data.json', headers={'If-None-Match': headers['ETag']})[0], 304)
        self.assertEqual(self.request('/data.json', headers={'If-None-Match': '"old", W/' + headers['ETag']})[0], 304)
        self.assertEqual(self.request('/data.json', headers={'If-Modified-Since': headers['Last-Modified']})[0], 304)
        self.assertEqual(self.request('/data.json', headers={'Range': 'bytes=0-1', 'If-Range': headers['Last-Modified']})[0], 206)
        self.assertEqual(self.request('/data.json', headers={'If-None-Match': '"old"', 'If-Modified-Since': headers['Last-Modified']})[0], 200)
        self.assertEqual(self.request('/data.json', headers={'Range': 'bytes=0-1', 'If-Range': '"old"'})[2], body)
        for path in ('/%', '/%0', '/%GG', '/%2fetc/passwd', '/%2e%2e/secret.txt'):
            self.assertNotEqual(self.request(path)[0], 200)


if __name__ == '__main__':
    unittest.main()
