import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.validate_app import validate


class ManifestTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='reaweb-manifest-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.path = self.root / 'app.json'
        (self.root / 'index.html').write_text('<!doctype html>', encoding='utf-8')

    def write(self, **fields):
        self.path.write_text(json.dumps({'name': 'Audio Tool', 'version': '1.0.0', 'entry': 'index.html', **fields}), encoding='utf-8')

    def test_valid_manifest_is_read_only(self):
        self.write()
        before = self.path.read_bytes(), self.path.stat().st_mtime_ns
        self.assertEqual(validate(self.path)[1], self.root.resolve() / 'index.html')
        self.assertEqual(before, (self.path.read_bytes(), self.path.stat().st_mtime_ns))

    def test_shipped_example_manifests(self):
        examples = Path(__file__).resolve().parents[1] / 'examples'
        paths = sorted(path for group in ('main-demo', 'backends', 'integrations', 'windows')
                       for path in (examples / group).rglob('app.json'))
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(app=path.relative_to(examples).as_posix()):
                validate(path)

    def test_daw_keyboard_boolean(self):
        for enabled in (True, False):
            self.write(dawKeyboard=enabled)
            before = self.path.read_bytes()
            self.assertIs(validate(self.path)[0]['dawKeyboard'], enabled)
            self.assertEqual(self.path.read_bytes(), before)
        for value in ('true', 0, 1, None, [], {}):
            self.write(dawKeyboard=value)
            with self.assertRaises(ValueError):
                validate(self.path)

    def test_invalid_fields_and_entry_paths(self):
        for fields in ({'permissions': []}, {'schemaVersion': True}, {'version': 'latest'}, {'name': ''},
                       {'entry': '../index.html'}, {'entry': 'C:/index.html'}, {'entry': '/index.html'}, {'entry': 'missing.html'}):
            with self.subTest(fields=fields):
                self.write(**fields)
                with self.assertRaises((ValueError, OSError)):
                    validate(self.path)

    def test_duplicate_keys_are_rejected(self):
        self.path.write_text('{"name":"a","name":"b","version":"1.0.0","entry":"index.html"}')
        with self.assertRaises(ValueError):
            validate(self.path)

    def test_app_identity(self):
        self.write(id='timefold')
        self.assertEqual(validate(self.path)[0]['id'], 'timefold')
        for identity in ('', 'TimeFold', 'a_b', 'a.b', '../a', '音', None, 1):
            self.write(id=identity)
            with self.assertRaises(ValueError):
                validate(self.path)
