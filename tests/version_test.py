"""Version and packaging regressions. Native payloads here are test fixtures only."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.package_sdk import stage_sdk
from tools.version import source_version


class VersionTests(unittest.TestCase):
    def test_complete_cmake_version(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for version in ('1', '1.2', '0.1.8', '0.1.8.3', '12.34.56.789'):
                with self.subTest(version=version):
                    (root / 'CMakeLists.txt').write_text(
                        '# project(ReaWebAPI VERSION 0.0.0 LANGUAGES CXX)\n'
                        f'project(\n  ReaWebAPI\n  VERSION {version}\n  LANGUAGES CXX)\n', encoding='utf-8-sig')
                    self.assertEqual(source_version(root), version)

    def test_invalid_versions_are_rejected_instead_of_truncated(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for version in ('0.1.8.3.1', '0.1.8-beta', '0.1.8.', 'v0.1.8', '${VERSION}', '-1.2.3'):
                with self.subTest(version=version):
                    (root / 'CMakeLists.txt').write_text(
                        f'project(ReaWebAPI VERSION {version} LANGUAGES CXX)\n', encoding='utf-8')
                    with self.assertRaisesRegex(ValueError, 'CMakeLists.txt'):
                        source_version(root)

    @unittest.skipUnless(shutil.which('cmake'), 'CMake is not installed')
    def test_four_part_version_matches_cmake(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.24)\n'
                'project(ReaWebAPI VERSION 0.1.8.3 LANGUAGES NONE)\n', encoding='utf-8')
            subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build')],
                           check=True, capture_output=True)
            cache = (root / 'build/CMakeCache.txt').read_text(encoding='utf-8')
            self.assertIn(f'CMAKE_PROJECT_VERSION:STATIC={source_version(root)}\n', cache)

    def test_platform_package_cli_uses_source_version_everywhere(self):
        version = source_version()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stage, output = root / 'stage', root / 'release'
            binary = stage / 'UserPlugins/reaper_reawebapi-x64.dll'
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b'TEST FIXTURE - NOT A NATIVE EXTENSION')
            shutil.copytree(ROOT / 'demo', stage / 'Scripts/ReaWebAPI/Example')
            stage_sdk(stage, version)
            result = subprocess.run([
                sys.executable, str(ROOT / 'tools/package.py'), '--stage', str(stage),
                '--platform', 'windows', '--arch', 'x64', '--revision', 'test-version',
                '--release-dir', str(output)], cwd=ROOT, capture_output=True, text=True,
                encoding='utf-8', env={**os.environ, 'PYTHONUTF8': '1'})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(sorted(p.name for p in output.glob('*.zip')),
                             [f'ReaWebAPI-windows-x64-v{version}.zip'])
            with zipfile.ZipFile(output / f'ReaWebAPI-windows-x64-v{version}.zip') as archive:
                for name in ('ReaWebAPI/BUILD.json', 'ReaWebAPI/SDK/BUILD.json'):
                    metadata = json.loads(archive.read(name))
                    self.assertEqual(metadata['version'], version)
                    self.assertEqual(metadata['revision'], 'test-version')
                self.assertTrue(archive.read('ReaWebAPI/docs/release-notes.md')
                                .startswith(f'# ReaWebAPI v{version}\n'.encode()))


if __name__ == '__main__':
    unittest.main()
