import hashlib
import json
from pathlib import Path
import posixpath
import re
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.package_sdk import build_sdk, payload
from tools.clean_retired_mirror import RETIRED, clean_stage


def check_links(files):
    for path, data in files.items():
        if not path.endswith('.md'):
            continue
        text = data.decode('utf-8-sig')
        for target in re.findall(r'\]\(([^)]+)\)', text):
            if re.match(r'\w+://', target):
                continue
            page, _, anchor = target.partition('#')
            name = posixpath.normpath(posixpath.join(posixpath.dirname(path), page)) if page else path
            assert name in files, f'{path}: missing link target {target}'
            if anchor:
                headings = re.findall(r'^#+\s+(.+)$', files[name].decode('utf-8-sig'), re.M)
                slugs = {re.sub(r'[^\w\s-]', '', h.strip().lower()).replace(' ', '-') for h in headings}
                assert anchor in slugs, f'{path}: missing anchor {target}'


class SdkTests(unittest.TestCase):
    def test_complete_reference_and_source_links(self):
        files = {p.relative_to(ROOT).as_posix(): p.read_bytes() for directory in ('docs', 'runtime', 'api', 'examples')
                 for p in (ROOT / directory).rglob('*') if p != ROOT / 'docs/VALIDATION.md'
                 and not p.is_relative_to(ROOT / 'docs/proposals')
                 and 'node_modules' not in p.parts and 'dist' not in p.parts and p.is_file() and p.suffix in ('.md', '.ts', '.mts', '.js', '.mjs', '.json', '.lua', '.html', '.css')}
        files.update({n: (ROOT / n).read_bytes() for n in ('README.md', 'README.zh-CN.md', 'THIRD_PARTY.md', 'LICENSE.md', 'COPYING', 'COPYING.LESSER')})
        files.update({n: (ROOT / n).read_bytes() for n in ('src/public/reaweb_service.h', 'src/public/reaweb_stream.h', 'src/public/reaweb_tasks.h', 'tests/native_service_extension.cpp', 'tests/native_stream_extension.cpp')})
        check_links(files)

    def test_sdk_does_not_require_local_validation(self):
        read_bytes = Path.read_bytes

        def read_without_validation(path):
            if path.parent == ROOT / 'docs' and (path.name.startswith(('VALIDATION', 'SMOKE_TEST')) or '-isolation-validation' in path.name):
                raise FileNotFoundError(path)
            return read_bytes(path)

        with patch.object(Path, 'read_bytes', read_without_validation):
            files = payload(ROOT, '0.1.8.3', 'test-sdk')
        self.assertFalse(any(name.startswith('ReaWebAPI/docs/') and
                             ('-isolation-validation' in name or Path(name).name.startswith(('VALIDATION', 'SMOKE_TEST')))
                             for name in files))
        check_links(files)

    def test_upgrade_preserves_modified_files_and_user_content(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            old = root / 'previous.zip'
            with zipfile.ZipFile(old, 'w') as archive:
                for name in RETIRED:
                    archive.writestr(name, b'old release')
            with zipfile.ZipFile(old) as archive:
                archive.extractall(root)
                modified = root / RETIRED[0]
                modified.write_bytes(b'user changes')
                user_file = root / 'ReaWebAPI/SDK/api/custom.json'
                user_file.write_bytes(b'user data')
                self.assertEqual(clean_stage(root, archive), [RETIRED[0]])
                self.assertEqual(modified.read_bytes(), b'user changes')
                self.assertEqual(user_file.read_bytes(), b'user data')
                self.assertFalse(any((root / name).exists() for name in RETIRED[1:]))

    def test_standalone_sdk_content_links_and_checksums(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = build_sdk(Path(directory), '0.1.8.3', 'test-sdk')
            self.assertEqual(archive.name, 'ReaWebAPI-SDK-v0.1.8.3.zip')
            with zipfile.ZipFile(archive) as z:
                files = {name: z.read(name) for name in z.namelist()}
                self.assertFalse(any(name.startswith('ReaWebAPI/docs/') and
                             ('-isolation-validation' in name or Path(name).name.startswith(('VALIDATION', 'SMOKE_TEST')))
                             for name in files))
                self.assertFalse(any(name.startswith('UserPlugins/') or name.endswith(('.dll', '.so', '.dylib')) for name in files))
                metadata = json.loads(files['ReaWebAPI/SDK/BUILD.json'])
                self.assertEqual(metadata['version'], '0.1.8.3')
                self.assertTrue(files['ReaWebAPI/docs/release-notes.md'].startswith(b'# ReaWebAPI v0.1.8.3\n'))
                self.assertNotIn('externalAPI', metadata)
                self.assertTrue(set(RETIRED).isdisjoint(files))
                self.assertEqual(metadata['revision'], 'test-sdk')
                self.assertIn('ReaWebAPI/SDK/web-runtime/modules/空 格#%.js', files)
                self.assertIn(b'type="module"', files['ReaWebAPI/SDK/web-runtime/index.html'])
                self.assertIn('ReaWebAPI/SDK/runtime-api.d.ts', files)
                self.assertIn('ReaWebAPI/docs/runtime-api-inventory.md', files)
                self.assertIn('ReaWebAPI/SDK/runtime-demo/app.js', files)
                self.assertIn('ReaWebAPI/SDK/lua-backend/reawebapi-lua-backend.lua', files)
                for demo in ('eel-backend', 'eel-mixer'):
                    for name in (f'reawebapi-{demo}.eel', 'index.html', 'app.js', 'style.css', 'app.json', 'README.md', 'README.zh-CN.md'):
                        content = (ROOT / 'examples/backends' / demo / name).read_bytes()
                        if name.endswith('.md'):
                            content = content.replace(b'../../../docs/', b'../../docs/')
                        self.assertEqual(files[f'ReaWebAPI/SDK/{demo}/{name}'], content)
                self.assertIn('ReaWebAPI/SDK/tools/validate_app.py', files)
                for app in ('starter', 'runtime-demo', 'web-runtime', 'modern/public', 'lua-backend', 'eel-backend', 'eel-mixer', 'native-service', 'native-stream'):
                    manifest = json.loads(files[f'ReaWebAPI/SDK/{app}/app.json'])
                    self.assertRegex(manifest['id'], r'^[a-z0-9-]+$')
                for name in ('LICENSE.md', 'COPYING', 'COPYING.LESSER', 'THIRD_PARTY.md'):
                    self.assertEqual(files[f'ReaWebAPI/{name}'], (ROOT / name).read_bytes())
                for line in files['SHA256SUMS.txt'].decode().splitlines():
                    digest, name = line.split('  ', 1)
                    self.assertEqual(hashlib.sha256(files[name]).hexdigest(), digest)
                check_links(files)
                self.assertEqual(files['ReaWebAPI/SDK/starter/app.js'], (ROOT / 'examples/templates/starter/app.js').read_bytes())
                config = json.loads(files['ReaWebAPI/SDK/starter/jsconfig.json'])
                self.assertIn('../*.d.ts', config['include'])
                modern = json.loads(files['ReaWebAPI/SDK/modern/tsconfig.json'])
                self.assertIn('../*.d.ts', modern['include'])
                self.assertIn(b'path="../reaper.d.ts"', files['ReaWebAPI/SDK/runtime-demo/app.js'])


if __name__ == '__main__':
    unittest.main()
