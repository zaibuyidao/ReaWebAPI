import hashlib
import json
from pathlib import Path
import posixpath
import re
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.package_sdk import build_sdk, payload
from tools.api_sync.generate import verify


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
        schema = json.loads((ROOT / 'api/reaper_api.json').read_text())
        manifest = json.loads((ROOT / 'api/bindings.json').read_text())
        verify(ROOT, schema, manifest)
        doc = (ROOT / 'docs/api-reference.md').read_text(encoding='utf-8')
        headings = re.findall(r'^### (\w+)$', doc, re.M)
        self.assertEqual(set(headings), set(schema['functions']))
        self.assertEqual(len(headings), 730)
        files = {p.relative_to(ROOT).as_posix(): p.read_bytes() for directory in ('docs', 'runtime', 'api')
                 for p in (ROOT / directory).rglob('*') if 'node_modules' not in p.parts and 'dist' not in p.parts and p.is_file() and p.suffix in ('.md', '.ts', '.js', '.json', '.lua', '.html', '.css')}
        files.update({n: (ROOT / n).read_bytes() for n in ('README.md', 'README.zh-CN.md', 'THIRD_PARTY.md', 'LICENSE.md', 'COPYING', 'COPYING.LESSER')})
        check_links(files)

    def test_batch_declarations_match_native_registry(self):
        source = (ROOT / 'src/core/batch.hpp').read_text(encoding='utf-8')
        native = set(re.findall(r'"([A-Za-z][A-Za-z0-9_]+)"', source))
        types = (ROOT / 'runtime/reaper.d.ts').read_text(encoding='utf-8')
        union = types.split('type ReaWebBatchMethod =', 1)[1].split(';', 1)[0]
        declared = set(re.findall(r"'([^']+)'", union))
        self.assertEqual(native, declared)
        schema = json.loads((ROOT / 'api/reaper_api.json').read_text(encoding='utf-8'))
        self.assertTrue(native <= set(schema['functions']))
        self.assertEqual(len(native), 173)

    def test_standalone_sdk_content_links_and_checksums(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = build_sdk(Path(directory), '0.1.8.3', 'test-sdk')
            self.assertEqual(archive.name, 'ReaWebAPI-SDK-v0.1.8.3.zip')
            with zipfile.ZipFile(archive) as z:
                files = {name: z.read(name) for name in z.namelist()}
                self.assertFalse(any(name.startswith('UserPlugins/') or name.endswith(('.dll', '.so', '.dylib')) for name in files))
                metadata = json.loads(files['ReaWebAPI/SDK/BUILD.json'])
                self.assertEqual(metadata['version'], '0.1.8.3')
                self.assertTrue(files['ReaWebAPI/docs/release-notes.md'].startswith(b'# ReaWebAPI v0.1.8.3\n'))
                self.assertEqual(metadata['bindings'], 730)
                self.assertEqual(metadata['revision'], 'test-sdk')
                self.assertIn('ReaWebAPI/SDK/web-runtime/modules/空 格#%.js', files)
                self.assertIn(b'type="module"', files['ReaWebAPI/SDK/web-runtime/index.html'])
                self.assertIn('ReaWebAPI/SDK/runtime-api.d.ts', files)
                self.assertIn('ReaWebAPI/docs/runtime-api-inventory.md', files)
                self.assertIn('ReaWebAPI/SDK/runtime-demo/app.js', files)
                self.assertIn('ReaWebAPI/SDK/tools/validate_app.py', files)
                for name in ('LICENSE.md', 'COPYING', 'COPYING.LESSER', 'THIRD_PARTY.md'):
                    self.assertEqual(files[f'ReaWebAPI/{name}'], (ROOT / name).read_bytes())
                for line in files['SHA256SUMS.txt'].decode().splitlines():
                    digest, name = line.split('  ', 1)
                    self.assertEqual(hashlib.sha256(files[name]).hexdigest(), digest)
                check_links(files)
                self.assertEqual(files['ReaWebAPI/SDK/starter/app.js'], (ROOT / 'runtime/starter/app.js').read_bytes())
                config = json.loads(files['ReaWebAPI/SDK/starter/jsconfig.json'])
                self.assertIn('../*.d.ts', config['include'])


if __name__ == '__main__':
    unittest.main()
