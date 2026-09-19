"""Local release regression tests. Fixtures are synthetic and never distributed."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location('release', Path(__file__).parents[1] / 'tools/release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)

class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.version = release.source_version()
        for platform, arch, _, name in release.TARGETS:
            files = [name] + ([f'reawebapi-webview-{arch}'] if platform == 'linux' else [])
            with zipfile.ZipFile(self.directory / f'ReaWebAPI-{platform}-{arch}-v{self.version}.zip', 'w') as bundle:
                metadata = dict(version=self.version, platform=platform, architecture=arch, revision='test-sha', extension=name)
                bundle.writestr('ReaWebAPI/BUILD.json', json.dumps(metadata))
                for item in files:
                    data = ('TEST FIXTURE ' + item).encode()
                    (self.directory / item).write_bytes(data)
                    bundle.writestr('UserPlugins/' + item, data)
    def tearDown(self):
        self.temp.cleanup()
    def test_exact_reapack_layout(self):
        assets = release.assemble(self.directory, self.version, 'test-sha')
        self.assertEqual(len(assets), 16)
        self.assertIn(self.directory / f'ReaWebAPI-SDK-v{self.version}.zip', assets)
        with zipfile.ZipFile(self.directory / f'ReaWebAPI-ReaPack-v{self.version}.zip') as bundle:
            notices = {'LICENSE.md', 'COPYING', 'COPYING.LESSER', 'THIRD_PARTY.md'}
            self.assertEqual(set(bundle.namelist()), {'ReaWebAPI.ext'} | notices | {f'extension/{name}' for _, name in release.native_files()})
            for name in notices:
                self.assertEqual(bundle.read(name), (Path(__file__).parents[1] / name).read_bytes())
            self.assertNotIn('web/', bundle.read('ReaWebAPI.ext').decode())
            self.assertEqual(bundle.read('ReaWebAPI.ext').decode().count(' extension] '), 7)
            self.assertIn(f'@version {self.version}\n', bundle.read('ReaWebAPI.ext').decode())
        body = release.release_body('test/repo', self.version)
        links = release.re.findall(r'https://github.com/test/repo/releases/download/v[^/]+/([^\s)]+)', body)
        expected = {asset.name for asset in assets if asset.suffix == '.zip' or asset.name == 'SHA256SUMS.txt'}
        self.assertEqual(set(links), expected)
    def test_release_notes_select_exact_version(self):
        text = '# ReaWebAPI v1.2.3\n\n## Changes\n\n- Current release.\n\n# ReaWebAPI v1.2.2\n\n- Previous release.\n'
        with patch.object(release.Path, 'read_text', return_value=text):
            self.assertEqual(release.version_notes('1.2.3'), '## Changes\n\n- Current release.')
            with self.assertRaises(ValueError): release.version_notes('1.2')
    def test_four_part_release_notes_do_not_match_three_parts(self):
        text = '# ReaWebAPI v0.1.8.3\n\n- Four-part release.\n'
        with patch.object(release.Path, 'read_text', return_value=text):
            self.assertEqual(release.version_notes('0.1.8.3'), '- Four-part release.')
            with self.assertRaises(ValueError): release.version_notes('0.1.8')
    def test_current_release_notes_follow_cmake_version(self):
        text = '# ReaWebAPI\n\n## Changes\n\n- Current release.\n'
        with patch.object(release.Path, 'read_text', return_value=text):
            for version in ('0.1.8', '0.1.8.3', '0.2.0'):
                body = release.release_body('test/repo', version)
                self.assertTrue(body.startswith('## Changes\n\n- Current release.'))
                self.assertIn(f'/releases/download/v{version}/', body)
    def test_missing_helper_blocks_release(self):
        (self.directory / 'reawebapi-webview-aarch64').unlink()
        with self.assertRaises(ValueError): release.assemble(self.directory, self.version, 'test-sha')
    def test_mismatched_binary_blocks_release(self):
        (self.directory / 'reaper_reawebapi-x64.dll').write_bytes(b'wrong')
        with self.assertRaises(ValueError): release.assemble(self.directory, self.version, 'test-sha')
    def test_wrong_commit_blocks_release(self):
        with self.assertRaises(ValueError): release.assemble(self.directory, self.version, 'other-sha')
    def test_draft_uploaded_before_publish(self):
        calls = []
        original = release.gh
        def mock(*args):
            calls.append(args)
            if args[0] == 'api':
                raise subprocess.CalledProcessError(1, args, stderr='HTTP 404')
            return ''
        release.gh = mock
        try:
            release.publish('test/repo', self.version, 'test-sha', [self.directory / 'reaper_reawebapi-x64.dll'])
        finally:
            release.gh = original
        self.assertEqual([c[:2] for c in calls[2:]], [('release', 'create'), ('release', 'upload'), ('release', 'edit')])
        self.assertIn('--draft', calls[2])
        self.assertIn('--draft=false', calls[-1])
        self.assertEqual(calls[2][2], f'v{self.version}')

if __name__ == '__main__': unittest.main()
