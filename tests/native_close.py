"""Run macOS/Linux close acceptance in a disposable REAPER resource directory."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
mac = platform.system() == 'Darwin'
if not mac:
    assert os.environ.get('REAWEB_TEST_ISOLATED_DISPLAY') == '1', 'Use a dedicated X11 display'
build = Path(os.environ.get('REAWEB_TEST_BUILD_DIR', str(root / ('build-arm64' if mac else 'build-linux'))))
reaper = os.environ.get('REAWEB_TEST_REAPER', '/Applications/REAPER.app/Contents/MacOS/REAPER' if mac else '')
assert reaper, 'Set REAWEB_TEST_REAPER'
profile = Path(tempfile.mkdtemp(prefix='close-acceptance-', dir=build))
plugins = profile / 'UserPlugins'
plugins.mkdir()
arch = platform.machine()
suffix = 'dylib' if mac else 'so'
files = [build / f'reaper_reawebapi-{arch}.{suffix}', build / 'tests' / f'reaper_close_test.{suffix}']
if not mac:
    files.append(build / f'reawebapi-webview-{arch}')
for source in files:
    target = plugins / source.name
    shutil.copy2(source, target)
    if mac:
        subprocess.run(['codesign', '--force', '--sign', '-', str(target)], check=True, capture_output=True)
(profile / 'close-test.enabled').touch()
(profile / 'reaper.ini').write_text('[reaper]\nmixwnd_vis=0\nverchk=0\nerrnowarn=5\nloadlastproj=0\nshowlastproj=0\n'
    'splash=0\nhasrecentlyopened=1\nlinux_audio_mode=3\nlinux_audio_srate=48000\nlinux_audio_bsize=512\n'
    'linux_audio_nch_out=2\n[audioconfig]\nmode=4\n')
print('PROFILE', profile, flush=True)
with (profile / 'process.log').open('w') as output:
    process = subprocess.Popen([reaper, '-newinst', '-cfgfile', str(profile / 'reaper.ini'), '-new', '-nosplash'],
                               stdout=output, stderr=output)
    try:
        process.wait(timeout=150)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=10)
        raise
result = (profile / 'close-test.log').read_text()
print(result, flush=True)
assert 'PASS: all close cases' in result and 'FAIL:' not in result, result
