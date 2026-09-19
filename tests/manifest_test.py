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
