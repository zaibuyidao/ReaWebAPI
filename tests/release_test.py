"""Local release regression tests. Fixtures are synthetic and never distributed."""
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import unquote
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
            demo = Path(__file__).parents[1] / 'web'
            demo_files = {f'web/{path.relative_to(demo).as_posix()}': path.read_bytes()
                          for path in demo.rglob('*') if path.is_file()}
            files = {entry.filename for entry in bundle.infolist() if not entry.is_dir()}
            self.assertEqual(files, {'ReaWebAPI.ext'} | demo_files.keys() |
                             {f'extension/{name}' for _, name in release.native_files()})
            self.assertEqual({name for name in files if '/' not in name}, {'ReaWebAPI.ext'})
            for name, data in demo_files.items():
                self.assertEqual(bundle.read(name), data)
            for _, name in release.native_files():
                self.assertEqual(bundle.read(f'extension/{name}'), (self.directory / name).read_bytes())
                self.assertEqual(bundle.getinfo(f'extension/{name}').external_attr >> 16, 0o100755)
            descriptor = bundle.read('ReaWebAPI.ext').decode()
            self.assertNotIn('[data]', descriptor)
            entries = release.re.findall(r'^  \[script (main|nomain)\] (web/.+?) (https://\S+)$',
                                         descriptor, release.re.MULTILINE)
            self.assertEqual(len(entries), len(demo_files))
            self.assertEqual({target for _, target, _ in entries}, set(demo_files))
            self.assertCountEqual([target for mode, target, _ in entries if mode == 'main'],
                             ['web/ReaWebAPI_Demo.lua', 'web/lua-backend/Open.lua'])
            for _, target, url in entries:
                self.assertEqual(unquote(url),
                                 'https://raw.githubusercontent.com/zaibuyidao/ReaScripts/$commit/ReaWebAPI/' + target)
                self.assertEqual(bundle.read(target), demo_files[target])
            self.assertIn('-- @noindex', bundle.read('web/ReaWebAPI_Demo.lua').decode())
            self.assertEqual((self.directory / 'ReaWebAPI.ext').read_text(encoding='utf-8'), descriptor)
            changelog = descriptor.split('@changelog\n', 1)[1]
            # Follow this checkout's notes instead of hard-coding one release's prose or bullet count.
            english_notes = release.version_notes(self.version).split('## 简体中文', 1)[0].split('## 更新', 1)[0]
            expected_changes = ['  ' + line[2:] for line in english_notes.splitlines() if line.startswith('- ')]
            self.assertTrue(expected_changes)
            self.assertEqual(changelog.splitlines(), expected_changes)
            self.assertEqual(bundle.read('ReaWebAPI.ext').decode().count(' extension] '), 7)
            self.assertIn(f'@version {self.version}\n', bundle.read('ReaWebAPI.ext').decode())
        body = release.release_body('test/repo', self.version)
        links = release.re.findall(r'https://github.com/test/repo/releases/download/v[^/]+/([^\s)]+)', body)
        expected = {asset.name for asset in assets if asset.suffix == '.zip' or asset.name == 'SHA256SUMS.txt'}
        self.assertEqual(set(links), expected)
    def test_demo_includes_nested_hidden_and_binary_resources(self):
        demo = self.directory / 'source-demo'
        (demo / 'assets/empty').mkdir(parents=True)
        resources = {'index.html': b'<html>Demo</html>', '.config': b'hidden',
                     'assets/音频.wav': bytes(range(256))}
        for name, data in resources.items():
            (demo / name).write_bytes(data)
        output = self.directory / 'demo-test.zip'
        with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as bundle:
            release.add_demo(bundle, demo)
        with zipfile.ZipFile(output) as bundle:
            self.assertEqual(set(bundle.namelist()), {'web/', 'web/assets/', 'web/assets/empty/'} |
                             {f'web/{name}' for name in resources})
            for name, data in resources.items():
                self.assertEqual(bundle.read(f'web/{name}'), data)
    def test_missing_demo_is_rejected(self):
        with zipfile.ZipFile(self.directory / 'demo-test.zip', 'w') as bundle:
            with self.assertRaisesRegex(ValueError, 'Missing Demo directory'):
                release.add_demo(bundle, self.directory / 'missing-demo')
    def test_descriptor_includes_new_nested_resources(self):
        demo = self.directory / 'demo'
        (demo / 'assets').mkdir(parents=True)
        (demo / 'ReaWebAPI_Demo.lua').write_text('-- @noindex\n', encoding='utf-8')
        (demo / 'assets/音频 #%.wav').write_bytes(bytes(range(256)))
        (demo / '.config').write_text('hidden', encoding='utf-8')
        descriptor = release.descriptor(self.version, demo)
        self.assertIn('[script nomain] web/.config ', descriptor)
        self.assertIn('[script nomain] web/assets/音频 #%.wav '
                      'https://raw.githubusercontent.com/zaibuyidao/ReaScripts/$commit/ReaWebAPI/web/'
                      'assets/%E9%9F%B3%E9%A2%91%20%23%25.wav\n', descriptor)
        self.assertEqual(descriptor.count('[script main]'), 1)
    def test_missing_demo_launcher_blocks_descriptor(self):
        demo = self.directory / 'demo'
        demo.mkdir()
        (demo / 'index.html').write_text('<html>Demo</html>', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'Missing Demo launcher'):
            release.descriptor(self.version, demo)
    def test_descriptor_changelog_uses_only_selected_english_notes(self):
        for english_heading, chinese_heading in [('English', '简体中文'), ('Changes', '更新')]:
            for changes in [('First change.',), ('First change.', 'Second change.', 'Third change.')]:
                with self.subTest(english_heading=english_heading, changes=changes):
                    bullets = '\n'.join('- ' + change for change in changes)
                    notes = (f'# ReaWebAPI v{self.version}\n\n## {english_heading}\n\n{bullets}\n\n'
                             f'## {chinese_heading}\n\n- 中文更新。\n\n'
                             '# ReaWebAPI v0.0.0\n\n## English\n\n- Previous release.\n')
                    with patch.object(release.Path, 'read_text', return_value=notes):
                        descriptor = release.descriptor(self.version)
                    self.assertEqual(descriptor.split('@changelog\n', 1)[1],
                                     ''.join('  ' + change + '\n' for change in changes))
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
        def mock(*args):
            calls.append(args)
            if args[0] == 'api':
                if '--slurp' in args:
                    return '[[]]'
                raise subprocess.CalledProcessError(1, args, stderr='HTTP 404')
            return ''
        with patch.object(release, 'gh', side_effect=mock):
            release.publish('test/repo', self.version, 'test-sha', [self.directory / 'reaper_reawebapi-x64.dll'])
        self.assertEqual([c[:2] for c in calls[3:]], [('release', 'create'), ('release', 'upload'), ('release', 'edit')])
        self.assertIn('--draft', calls[3])
        self.assertIn('--draft=false', calls[-1])
        self.assertEqual(calls[3][2], f'v{self.version}')

    def test_resume_draft_missing_from_tag_lookup(self):
        draft = dict(tag_name=f'v{self.version}', draft=True, target_commitish='test-sha')
        responses = [subprocess.CalledProcessError(1, ['gh'], stderr='HTTP 404'),
                     json.dumps([[dict(tag_name='v0.0.0')], [draft]]),
                     json.dumps(dict(sha='test-sha')), '', '', '']
        with patch.object(release, 'gh', side_effect=responses) as gh:
            release.publish('test/repo', self.version, 'test-sha', [self.directory / 'reaper_reawebapi-x64.dll'])
        commands = [call.args for call in gh.call_args_list]
        self.assertIn('--paginate', commands[1])
        self.assertNotIn(('release', 'create'), [cmd[:2] for cmd in commands])
        self.assertIn('--notes-file', commands[3])
        self.assertIn('--clobber', commands[4])
        self.assertIn('--draft=false', commands[5])

    def test_create_timeout_resumes_accepted_draft(self):
        draft = dict(tag_name=f'v{self.version}', draft=True, target_commitish='test-sha')
        missing = subprocess.CalledProcessError(1, ['gh'], stderr='HTTP 404')
        responses = [missing, '[[]]', missing,
                     subprocess.CalledProcessError(1, ['gh', 'release', 'create'], stderr='HTTP 502: Bad Gateway'),
                     missing, json.dumps([[draft]]), json.dumps(dict(sha='test-sha')), '', '', '']
        with patch.object(release, 'gh', side_effect=responses) as gh, \
             patch.object(release.time, 'sleep') as sleep, patch('sys.stderr', new_callable=io.StringIO) as output:
            release.publish('test/repo', self.version, 'test-sha', [self.directory / 'reaper_reawebapi-x64.dll'])
        self.assertEqual(sum(call.args[:2] == ('release', 'create') for call in gh.call_args_list), 1)
        sleep.assert_called_once_with(5)
        self.assertIn('HTTP 502: Bad Gateway', output.getvalue())
        self.assertIn('--draft=false', gh.call_args_list[-1].args)

    def test_failed_upload_is_retried_before_publishing(self):
        draft = json.dumps(dict(tag_name=f'v{self.version}', draft=True, target_commitish='test-sha'))
        commit = json.dumps(dict(sha='test-sha'))
        responses = [draft, commit, '',
                     subprocess.CalledProcessError(1, ['gh', 'release', 'upload'], stderr='connection reset by peer'),
                     draft, commit, '', '', '']
        with patch.object(release, 'gh', side_effect=responses) as gh, \
             patch.object(release.time, 'sleep'), patch('sys.stderr', new_callable=io.StringIO):
            release.publish('test/repo', self.version, 'test-sha', [self.directory / 'reaper_reawebapi-x64.dll'])
        commands = [call.args for call in gh.call_args_list]
        self.assertEqual(sum(cmd[:2] == ('release', 'upload') for cmd in commands), 2)
        self.assertEqual(sum('--draft=false' in cmd for cmd in commands), 1)
        self.assertIn('--draft=false', commands[-1])

    def test_published_release_is_not_overwritten(self):
        with patch.object(release, 'gh', return_value=json.dumps(dict(draft=False))) as gh:
            release.publish('test/repo', self.version, 'test-sha', [self.directory / 'reaper_reawebapi-x64.dll'])
        gh.assert_called_once()

    def test_existing_tag_or_draft_at_different_commit_is_rejected(self):
        for tagged, target in [('other-sha', 'test-sha'), ('test-sha', 'other-sha')]:
            with self.subTest(tagged=tagged, target=target):
                responses = [json.dumps(dict(draft=True, target_commitish=target)), json.dumps(dict(sha=tagged))]
                with patch.object(release, 'gh', side_effect=responses) as gh:
                    with self.assertRaisesRegex(ValueError, 'different commit'):
                        release.publish('test/repo', self.version, 'test-sha', [self.directory / 'reaper_reawebapi-x64.dll'])
                self.assertEqual(gh.call_count, 2)

    def test_permission_error_is_reported_without_retry(self):
        error = subprocess.CalledProcessError(1, ['gh'], stderr='HTTP 403: Resource not accessible by integration')
        with patch.object(release, 'gh', side_effect=error) as gh, \
             patch.object(release.time, 'sleep') as sleep, patch('sys.stderr', new_callable=io.StringIO) as output:
            with self.assertRaises(subprocess.CalledProcessError):
                release.publish('test/repo', self.version, 'test-sha', [self.directory / 'reaper_reawebapi-x64.dll'])
        gh.assert_called_once()
        sleep.assert_not_called()
        self.assertIn(error.stderr, output.getvalue())

    def test_transient_errors_have_bounded_retries(self):
        error = subprocess.CalledProcessError(1, ['gh'], stderr='HTTP 503: Service Unavailable')
        with patch.object(release, 'gh', side_effect=error) as gh, \
             patch.object(release.time, 'sleep') as sleep, patch('sys.stderr', new_callable=io.StringIO) as output:
            with self.assertRaises(subprocess.CalledProcessError):
                release.publish('test/repo', self.version, 'test-sha', [self.directory / 'reaper_reawebapi-x64.dll'])
        self.assertEqual(gh.call_count, 3)
        self.assertEqual([call.args[0] for call in sleep.call_args_list], [5, 10])
        self.assertIn('attempt 3 failed: HTTP 503', output.getvalue())

if __name__ == '__main__': unittest.main()
