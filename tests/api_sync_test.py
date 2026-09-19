import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.api_sync.sources import parse_html, parse_markdown, load_source, read_json
from tools.api_sync.model import normalize, normalize_function, compare, coverage, encode, validate_schema, js_type_names
from tools.api_sync.generate import write_files, verify

SCHEMA = read_json((ROOT / 'api/reaper_api.json').read_text())
BINDINGS = read_json((ROOT / 'api/bindings.json').read_text())


class SyncTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='reaweb-api-sync-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for directory in ('api', 'src', 'runtime', 'docs'):
            (self.root / directory).mkdir()
        for name in ('api/reaper_api.json', 'api/bindings.json', 'src/core.cpp',
                     'runtime/reaper-api.generated.js', 'runtime/reaper-api.generated.d.ts', 'runtime/reaper.d.ts', 'docs/api-reference.md'):
            shutil.copyfile(ROOT / name, self.root / name)

    def command(self, *args):
        return subprocess.run([sys.executable, '-m', 'tools.api_sync', '--root', str(self.root), *args],
                              cwd=ROOT, capture_output=True, text=True, encoding='utf-8',
                              env={**os.environ, 'PYTHONIOENCODING': 'utf-8'})

    def snapshot(self):
        return {str(p.relative_to(self.root)): (p.read_bytes(), p.stat().st_mtime_ns)
                for p in self.root.rglob('*') if p.is_file()}

    def assert_unchanged(self, before):
        after = self.snapshot()
        self.assertEqual(before.keys(), after.keys())
        for name, (content, mtime) in before.items():
            # Report the file, not a multi-megabyte diff of the whole catalogue.
            self.assertTrue(content == after[name][0], f'File content changed: {name}')
            self.assertEqual(mtime, after[name][1], f'File modification time changed: {name}')

    def source(self, functions=None, version='7.80'):
        path = self.root / 'input.json'
        path.write_text(encode({'reaperVersion': version, 'functions': functions or SCHEMA['functions']}), encoding='utf-8')
        return str(path)

    def test_offline_noop_and_read_only(self):
        for newline in (b'\n', b'\r\n'):
            with self.subTest(newline=newline):
                # Exercise both Git checkout styles on every CI platform.
                for path in self.root.rglob('*'):
                    if path.is_file():
                        content = path.read_bytes().replace(b'\r\n', b'\n')
                        path.write_bytes(content.replace(b'\n', newline))
                before = self.snapshot()
                for args in [('verify',), ('check', '--offline'), ('report', '--json'),
                             ('update', '--offline', '--json')]:
                    with self.subTest(command=args):
                        result = self.command(*args)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assert_unchanged(before)
                        if args[0] == 'update':
                            self.assertEqual(json.loads(result.stdout)['written'], [])
        report = json.loads(self.command('report', '--json').stdout)
        self.assertEqual(report['coverage']['official'], 730)
        self.assertEqual(report['coverage']['implemented'], 730)
        self.assertEqual(report['coverage']['missing'], 0)
        self.assertEqual(self.command('check', '--offline', '--require-complete').returncode, 0)

    def test_full_official_html_and_index_validation(self):
        cached = ROOT / '.cache/api_sync/reascripthelp.html'
        if not cached.exists(): self.skipTest('Official HTML cache unavailable')
        text = cached.read_text(encoding='utf-8')
        version, functions, excluded = parse_html(text)
        self.assertEqual(version, '7.80')
        self.assertEqual(len(functions), 730)
        schema = normalize(SCHEMA['source'], functions, excluded)
        self.assertEqual(schema, SCHEMA)
        value = schema['functions']['GetTrackName']
        self.assertEqual([r['type'] for r in value['returns']], ['boolean', 'string'])
        self.assertEqual(schema['functions']['EnsureNotCompletelyOffscreen']['parameters'][0]['name'], 'r.left')
        with self.assertRaises(ValueError): parse_html(text[:text.index('<a name="GetTrackName"')])
        with self.assertRaises(ValueError): parse_html(text.replace('name="GetTrackName"', 'name="GetAppVersion"'))

    def test_markdown_exchange_and_invalid_signatures(self):
        text = '<!-- reaper-version: 7.80 -->\n## Example\n```c\nbool Example(int count, char* bufOut, int bufOut_sz)\n```\n```lua\nboolean retval, string buf = reaper.Example(optional integer count)\n```\n'
        version, raw, _ = parse_markdown(text)
        entry = normalize_function('Example', raw['Example'])
        self.assertEqual(version, '7.80')
        self.assertEqual(entry['parameters'][0]['optional'], True)
        self.assertEqual(len(entry['returns']), 2)
        with self.assertRaises(ValueError): parse_markdown(text + text)
        with self.assertRaises(ValueError): normalize_function('WrongName', raw['Example'])
        with self.assertRaises(ValueError): read_json('{"x":1,"x":2}')

    def test_changes_and_binding_review_gate(self):
        functions = copy.deepcopy(SCHEMA['functions'])
        functions['GetAppVersion']['signatures']['lua'] = 'integer reaper.GetAppVersion()'
        functions['NewAPI'] = {'signatures': {'c': 'void NewAPI(int value)', 'lua': 'reaper.NewAPI(integer value)'}}
        location = self.source(functions, '7.81')
        result = self.command('check', '--source', location, '--json')
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report['difference']['added'], ['NewAPI'])
        self.assertIn('returns', report['difference']['changed']['GetAppVersion']['fields'])
        self.assertEqual(report['coverage']['needsReview'], ['GetAppVersion'])
        self.assertEqual(report['coverage']['implemented'], 729)
        native = (self.root / 'src/core.cpp').read_bytes()
        bindings = (self.root / 'api/bindings.json').read_bytes()
        result = self.command('update', '--source', location)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / 'src/core.cpp').read_bytes(), native)
        self.assertEqual((self.root / 'api/bindings.json').read_bytes(), bindings)
        self.assertEqual(self.command('verify').returncode, 2)
        before = self.snapshot()
        self.assertEqual(self.command('update', '--source', location).returncode, 0)
        self.assert_unchanged(before)
        self.assertTrue((self.root / 'api/changelog/7.81.md').exists())

    def test_removal_and_downgrade_guards(self):
        functions = copy.deepcopy(SCHEMA['functions'])
        del functions['APIExists']
        location = self.source(functions, '7.79')
        before = self.snapshot()
        self.assertEqual(self.command('update', '--source', location).returncode, 2)
        self.assert_unchanged(before)
        self.assertEqual(self.command('update', '--source', location, '--allow-removals').returncode, 2)
        self.assert_unchanged(before)
        self.assertEqual(self.command('update', '--source', location, '--allow-removals', '--allow-downgrade').returncode, 0)

    def test_bad_input_hash_and_version_write_nothing(self):
        location = self.source()
        before = self.snapshot()
        for extra in [('--sha256', '0'*64), ('--source-version', '7.81')]:
            self.assertEqual(self.command('update', '--source', location, *extra).returncode, 2)
            self.assert_unchanged(before)
        Path(location).write_text('{bad json')
        before = self.snapshot()
        self.assertEqual(self.command('update', '--source', location).returncode, 2)
        self.assert_unchanged(before)

    def test_generated_sdk_and_registry_drift_fail_verify(self):
        path = self.root / 'runtime/reaper-api.generated.d.ts'
        original = path.read_text(encoding='utf-8')
        for newline in ('\n', '\r\n'):
            with self.subTest(newline=newline):
                path.write_bytes(original.replace('Promise<string>', 'Promise<number>')
                                 .replace('\n', newline).encode('utf-8'))
                self.assertEqual(self.command('verify').returncode, 2)
                result = self.command('update', '--offline', '--json')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout)['written'], ['runtime/reaper-api.generated.d.ts'])
                self.assertEqual(path.read_bytes(), original.encode('utf-8'))
                self.assertEqual(self.command('verify').returncode, 0)
        native = self.root / 'src/core.cpp'
        native.write_text(native.read_text().replace('native_->invoke', 'unwired_native'))
        self.assertEqual(self.command('verify').returncode, 2)

    def test_documentation_only_does_not_invalidate_binding(self):
        new = copy.deepcopy(SCHEMA)
        new['functions']['GetAppVersion']['documentationHash'] = '0'*64
        self.assertEqual(compare(SCHEMA, new)['changed']['GetAppVersion']['fields'], ['documentationHash'])
        self.assertEqual(coverage(new, BINDINGS)['needsReview'], [])
        with self.assertRaises(ValueError): validate_schema(new)

    def test_same_version_changelog_appends(self):
        functions = copy.deepcopy(SCHEMA['functions'])
        for i in (1, 2):
            functions['APIExists']['documentationHash'] = str(i) * 64
            result = self.command('update', '--source', self.source(functions))
            self.assertEqual(result.returncode, 0, result.stderr)
        log = (self.root / 'api/changelog/7.80.md').read_text(encoding='utf-8')
        self.assertEqual(log.count('<!-- sync:'), 2)

    def test_write_rollback_on_replace_failure(self):
        a, b = self.root / 'a', self.root / 'b'
        a.write_text('original')
        original_replace = os.replace
        calls = 0
        def replace(source, dest):
            nonlocal calls
            calls += 1
            if calls == 2: raise OSError('simulated full disk')
            return original_replace(source, dest)
        with patch('tools.api_sync.generate.os.replace', replace), self.assertRaises(OSError):
            write_files({a:'new', b:'new'})
        self.assertEqual(a.read_text(), 'original')
        self.assertFalse(b.exists())
        self.assertFalse(list(self.root.glob('*.tmp')))

    def test_extensible_js_contract_types(self):
        self.assertEqual(js_type_names('[boolean, MediaTrackHandle | null, number[]]'), {'boolean', 'MediaTrackHandle', 'null', 'number'})
        for invalid in ('', 'string); alert(1)', '[number,]', 'number||string', '[number'):
            with self.assertRaises(ValueError): js_type_names(invalid)
        manifest = copy.deepcopy(BINDINGS)
        manifest['functions']['GetAppVersion']['returns'] = 'MissingHandle'
        (self.root / 'api/bindings.json').write_text(encode(manifest))
        self.assertEqual(self.command('verify').returncode, 2)


if __name__ == '__main__': unittest.main()
