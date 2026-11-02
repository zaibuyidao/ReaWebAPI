"""Persistent ordinary App identities against the native binding and resource implementation."""
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class AutoIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.root = self.base / 'App'
        self.root.mkdir()
        (self.root / 'index.html').write_text('<html></html>')
        self.apps = self.base / 'Apps'
        self.processes = []

    def tearDown(self):
        for process in self.processes:
            if process.poll() is None:
                process.communicate('\n', timeout=8)
        self.temp.cleanup()

    def marker(self, root=None):
        return (root or self.root) / '.reawebapi' / 'identity.json'

    def identity(self, root=None):
        data = json.loads(self.marker(root).read_text())
        self.assertEqual(data['schema'], 1)
        self.assertRegex(data['appId'], r'^app-[0-9a-f]{32}$')
        return data['appId']

    def command(self, root=None, source='@Same.lua', entry='index.html'):
        return [BINARY, str(root or self.root), str(self.apps), source, entry]

    def launch(self, root=None, source='@Same.lua', entry='index.html'):
        return subprocess.run(self.command(root, source, entry), input='\n', text=True,
                              capture_output=True, encoding='utf-8', timeout=10)

    def opened(self, root=None, source='@Same.lua', entry='index.html'):
        result = self.launch(root, source, entry)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), 'reaweb://' + self.identity(root))
        return self.identity(root)

    def start(self, root=None):
        process = subprocess.Popen(self.command(root), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   text=True, encoding='utf-8')
        self.processes.append(process)
        return process

    def copy(self, name='Copy'):
        other = self.base / name
        shutil.copytree(self.root, other)
        return other

    def test_create_restart_move_and_launcher_rename(self):
        identity = self.opened(source='')
        self.assertFalse((self.root / 'app.json').exists())
        self.assertEqual(self.opened(source='@Renamed.lua'), identity)
        data = self.apps / identity / 'Data' / 'retained'
        data.parent.mkdir(); data.write_text('retained')
        moved = self.base / 'Moved App'
        self.root.rename(moved)
        self.root = moved
        self.assertEqual(self.opened(), identity)
        self.assertEqual(data.read_text(), 'retained')
        self.assertEqual(self.opened(), identity)

    def test_manifest_without_id_and_non_index_entry(self):
        manifest = self.root / 'app.json'
        manifest.write_text('{"name":"Tool","version":"1.0.0","entry":"launch.htm"}')
        before = manifest.read_bytes()
        (self.root / 'index.html').rename(self.root / 'launch.htm')
        identity = self.opened(entry='launch.htm')
        self.assertEqual(self.opened(source='not a Lua filename', entry='launch.htm'), identity)
        self.assertEqual(manifest.read_bytes(), before)
        record = json.loads((self.apps / identity / 'origin.json').read_text())
        self.assertEqual(record['entry'], 'launch.htm')

    def test_independent_and_complete_copies(self):
        plain = self.copy('Plain')
        original = self.opened()
        self.assertNotEqual(self.opened(plain), original)
        copied = self.copy()
        self.assertEqual(self.identity(copied), original)
        clone = self.opened(copied)
        self.assertNotEqual(clone, original)
        self.assertEqual(self.opened(copied), clone)
        self.assertEqual(self.opened(), original)

    def test_concurrent_first_open_same_root(self):
        processes = [self.start(), self.start()]
        origins = [p.stdout.readline().strip() for p in processes]
        self.assertEqual(origins, ['reaweb://' + self.identity()] * 2)

    def test_concurrent_unbound_copies(self):
        marker = self.marker()
        marker.parent.mkdir()
        marker.write_text(json.dumps({'schema': 1, 'appId': 'app-' + 'a' * 32}))
        copied = self.copy()
        processes = [self.start(), self.start(copied)]
        origins = [p.stdout.readline().strip() for p in processes]
        self.assertNotEqual(origins[0], origins[1])
        ids = [self.identity(), self.identity(copied)]
        self.assertEqual(origins, ['reaweb://' + identity for identity in ids])
        self.assertIn('app-' + 'a' * 32, ids)
        for folder, identity in zip((self.root, copied), ids):
            record = json.loads((self.apps / identity / 'origin.json').read_text())
            self.assertTrue(Path(record['root']).samefile(folder))

    def test_copy_of_active_binding_gets_independent_id(self):
        process = self.start()
        original = process.stdout.readline().strip()
        copied = self.copy()
        self.assertNotEqual('reaweb://' + self.opened(copied), original)
        self.assertEqual('reaweb://' + self.identity(), original)

    def test_invalid_old_entry_recovers_same_id(self):
        original = self.opened()
        copied = self.copy()
        (self.root / 'index.html').unlink()
        (self.root / 'other.html').write_text('unrelated')
        self.assertEqual(self.opened(copied), original)

    def test_invalid_but_active_binding_does_not_change_marker(self):
        process = self.start()
        process.stdout.readline()
        copied = self.copy()
        before = self.marker(copied).read_bytes()
        (self.root / 'index.html').unlink()
        result = self.launch(copied)
        self.assertIn('APP_ID_CONFLICT', result.stdout)
        self.assertEqual(self.marker(copied).read_bytes(), before)
        process.communicate('\n', timeout=8)
        self.assertEqual(self.opened(copied), self.identity())

    def test_corrupt_marker_never_overwritten(self):
        marker = self.marker()
        marker.parent.mkdir()
        valid = {'schema': 1, 'appId': 'app-' + 'a' * 32}
        invalid = ['{broken', '[]', '{"schema":1,"schema":1,"appId":"app-' + 'a' * 32 + '"}']
        invalid += [json.dumps({**valid, **fields}) for fields in
                    ({'schema': 2}, {'schema': True}, {'appId': '../escape'}, {'appId': 'jsfx-123'},
                     {'appId': 123}, {'appId': 'app-' + 'A' * 32}, {'unexpected': 1})]
        for text in invalid:
            marker.write_text(text)
            self.assertIn('APP_IDENTITY_INVALID', self.launch().stdout)
            self.assertEqual(marker.read_text(), text)
        self.assertFalse(self.apps.exists())

    def test_explicit_id_ignores_marker_and_remains_authoritative(self):
        self.opened()
        marker = self.marker()
        marker.write_text('{broken')
        (self.root / 'app.json').write_text('{"id":"explicit-tool"}')
        self.assertEqual(self.launch().stdout.strip(), 'reaweb://explicit-tool')
        self.assertEqual(marker.read_text(), '{broken')
        copied = self.copy()
        self.assertIn('APP_ID_CONFLICT', self.launch(copied).stdout)

    def test_upgrade_does_not_consult_or_delete_launcher_identity(self):
        old = self.apps / 'same'
        old.mkdir(parents=True)
        (old / 'origin.json').write_text('corrupt legacy record must not be read')
        for name in ('Data', 'WindowState', 'WebViewData'):
            (old / name).mkdir(); (old / name / 'retained').write_text('old data')
        self.opened()
        self.assertEqual((old / 'origin.json').read_text(), 'corrupt legacy record must not be read')
        for name in ('Data', 'WindowState', 'WebViewData'):
            self.assertEqual((old / name / 'retained').read_text(), 'old data')

    def test_deleted_marker_initializes_new_identity(self):
        original = self.opened()
        old_record = (self.apps / original / 'origin.json').read_bytes()
        self.marker().unlink()
        self.assertNotEqual(self.opened(), original)
        self.assertEqual((self.apps / original / 'origin.json').read_bytes(), old_record)

    def test_unknown_binding_status_does_not_change_marker(self):
        original = self.opened()
        copied = self.copy()
        record = self.apps / original / 'origin.json'
        data = json.loads(record.read_text())
        data.pop('entry')
        record.write_text(json.dumps(data))
        before = self.marker(copied).read_bytes()
        self.assertIn('Missing entry metadata', self.launch(copied).stdout)
        self.assertEqual(self.marker(copied).read_bytes(), before)
        record.write_text('{broken')
        self.assertIn('APP_ORIGIN_INVALID', self.launch(copied).stdout)
        self.assertEqual(self.marker(copied).read_bytes(), before)

    def test_marker_not_served(self):
        process = self.start()
        origin = process.stdout.readline().strip()
        for path in ('/.reawebapi/identity.json', '/%2ereawebapi/identity.json', '/.reawebapi/.identity.lock'):
            for method in ('GET', 'HEAD'):
                process.stdin.write(json.dumps({'uri': origin + path, 'method': method}) + '\n')
                process.stdin.flush()
                self.assertEqual(json.loads(process.stdout.readline())['status'], 403)

    def test_marker_file_alias_not_served(self):
        process = self.start()
        origin = process.stdout.readline().strip()
        os.link(self.marker(), self.root / 'alias.json')
        paths = ['/alias.json']
        if (self.root / '.REAWebAPI' / 'identity.json').exists():
            paths.append('/.REAWebAPI/identity.json')
        for path in paths:
            process.stdin.write(json.dumps({'uri': origin + path}) + '\n')
            process.stdin.flush()
            self.assertEqual(json.loads(process.stdout.readline())['status'], 403)

    @unittest.skipUnless(sys.platform == 'win32', 'Windows junctions')
    def test_marker_directory_alias_not_served(self):
        process = self.start()
        origin = process.stdout.readline().strip()
        link = self.root / 'alias'
        subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
                        "$ErrorActionPreference = 'Stop'; New-Item -ItemType Junction "
                        '-Path $env:REAWEB_TEST_LINK -Target $env:REAWEB_TEST_TARGET | Out-Null'],
                       env={**os.environ, 'REAWEB_TEST_LINK': str(link),
                            'REAWEB_TEST_TARGET': str(self.marker().parent)},
                       check=True, capture_output=True, timeout=15)
        try:
            for path in ('/alias/identity.json', '/.REAWebAPI/identity.json'):
                process.stdin.write(json.dumps({'uri': origin + path}) + '\n')
                process.stdin.flush()
                self.assertEqual(json.loads(process.stdout.readline())['status'], 403)
        finally:
            link.rmdir()

    @unittest.skipUnless(sys.platform == 'win32', 'Windows file sharing')
    def test_unreadable_marker_does_not_generate_another_id(self):
        self.opened()
        marker = self.marker()
        before = marker.read_bytes()
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p,
                                      ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.CreateFileW(str(marker), 0x80000000, 0, None, 3, 0, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        try:
            self.assertIn('APP_IDENTITY_INVALID', self.launch().stdout)
        finally:
            kernel.CloseHandle(handle)
        self.assertEqual(marker.read_bytes(), before)

    def test_metadata_directory_cannot_be_a_symlink(self):
        outside = self.base / 'Outside'
        outside.mkdir()
        try:
            self.marker().parent.symlink_to(outside, target_is_directory=True)
        except OSError as error:
            if sys.platform == 'win32' and error.winerror == 1314:
                self.skipTest('Windows symlink privilege unavailable')
            raise
        try:
            self.assertIn('APP_IDENTITY_UNAVAILABLE', self.launch().stdout)
            self.assertEqual(list(outside.iterdir()), [])
        finally:
            self.marker().parent.unlink()

    def test_invalid_new_entry_does_not_create_marker(self):
        self.assertIn('INVALID_PATH', self.launch(entry='missing.htm').stdout)
        self.assertFalse(self.marker().exists())

    @unittest.skipUnless(sys.platform == 'win32', 'Windows access control')
    def test_read_only_root_cannot_initialize(self):
        import csv
        user = subprocess.check_output(['whoami', '/user', '/fo', 'csv', '/nh'], text=True)
        sid = '*' + next(csv.reader([user.strip()]))[-1]
        subprocess.run(['icacls', str(self.root), '/deny', sid + ':(OI)(CI)(WD,AD)'], check=True, capture_output=True)
        try:
            self.assertIn('APP_IDENTITY_UNAVAILABLE', self.launch().stdout)
            self.assertFalse(self.marker().exists())
        finally:
            subprocess.run(['icacls', str(self.root), '/remove:d', sid], check=True, capture_output=True)

    @unittest.skipIf(sys.platform == 'win32', 'POSIX access control')
    def test_posix_read_only_root_cannot_initialize(self):
        if os.geteuid() == 0:
            self.skipTest('Root bypasses permissions')
        self.root.chmod(0o500)
        try:
            self.assertIn('APP_IDENTITY_UNAVAILABLE', self.launch().stdout)
        finally:
            self.root.chmod(0o700)

    @unittest.skipUnless(sys.platform == 'win32', 'Windows read-only attribute')
    def test_copy_marker_update_failure_is_not_silent(self):
        original = self.opened()
        copied = self.copy()
        marker = self.marker(copied)
        before = marker.read_bytes()
        marker.chmod(0o444)
        try:
            self.assertIn('APP_IDENTITY_UNAVAILABLE', self.launch(copied).stdout)
            self.assertEqual(marker.read_bytes(), before)
            self.assertEqual(self.identity(), original)
        finally:
            marker.chmod(0o666)


if __name__ == '__main__':
    unittest.main()
