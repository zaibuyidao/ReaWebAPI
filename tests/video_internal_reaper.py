"""Opt-in REAPER internal video experiment in a new, disposable resource profile."""
import argparse
import collections
import configparser
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import time
import zlib

BUILDS = {
    ('win32', 'AMD64'): ('reaper_video.dll', '0143b8b78ee6bc714782411f89889d67470b07e94aa02d56922f54f76a1a2ec7'),
    ('darwin', 'arm64'): ('reaper_video.dylib', '96dc59911188d3c5ad9528f09b452d7b5174a3f60acf1140e0742e68040876c0'),
    ('linux', 'x86_64'): ('reaper_video.so', '9bc595cb8f14bf71e7a47b6eb8e95afdcb16d9e8dfd51f3d9ff799f664d8e095'),
}


def png(path, rgb):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 640, 360, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress((b'\0' + bytes(rgb) * 640) * 360)) + chunk(b'IEND', b''))


def validate(root, backend='grab'):
    rows = [json.loads(line) for line in (root / 'results.jsonl').read_text().splitlines()]
    assert rows and rows[-1].get('done'), 'Experiment did not complete'
    groups = collections.defaultdict(list)
    for row in rows:
        if 'phase' not in row:
            continue
        result = row['result']
        assert 'error' not in result, row
        assert row['monitorFX'] == (1 if row['phase'] == 'monitor_effect_closed' else 0), row
        if not row['phase'].startswith('open'):
            assert row['window'] == 0 and not result['nativeWindow'], row
        groups[row['phase']].append(result)
    for phase in ('cold_closed', 'cold_closed_seek', 'closed', 'closed_seek', 'closed_restored',
                  'closed_restored_seek', 'restore'):
        assert len(groups[phase]) == 20, phase
        assert not any(row['frame'] for row in groups[phase][-10:]), phase
    for phase, pixel in (
        ('cold_forced', [60, 30, 210]), ('cold_forced_seek', [220, 80, 30]),
        ('closed_forced', [60, 30, 210]), ('closed_forced_seek', [220, 80, 30]),
        ('master_overlay_closed', [220, 80, 30]),
    ):
        valid = [row for row in groups[phase] if row['frame']]
        assert len(valid) >= 10 and valid[-1]['pixel'] == pixel, (phase, valid[-1:])
    assert groups['master_overlay_closed'][-1]['corner'] == [0, 255, 0]
    if backend == 'grab':
        # Characterize the known cache mismatch with an existing monitoring video effect.
        assert not any(row['frame'] for row in groups['monitor_effect_closed'][-10:])
    else:
        assert groups['monitor_effect_closed'][-1]['frame']
        assert groups['monitor_effect_closed'][-1]['corner'] == [0, 255, 0]
    motion = [row for row in groups['motion_play_closed'] if row['frame']]
    assert len(groups['motion_play_closed']) == 180 and len(motion) >= 90
    assert len({row['hash'] for row in motion}) >= 30, 'Motion output is stale'
    seek = groups['motion_seek_closed'][-1]
    assert seek['frame'] and seek['w'] == 1280 and seek['h'] == 720 and abs(seek['time'] - 4) < .05, seek
    assert groups['cleanup'][-1]['visibleFlag'] == 0
    empty = groups['empty_project']
    assert len(empty) == 20
    if backend == 'display':
        assert all(row['frame'] and row['hash'] == seek['hash'] for row in empty[-10:]), 'Expected retained display frame'
    else:
        assert not any(row['frame'] for row in empty[-10:])
    summary = {phase: {'samples': len(items), 'frames': sum(row['frame'] for row in items),
                       'distinctFrames': len({row['hash'] for row in items if row['frame']}),
                       'maxPullMs': max(row['pullMs'] for row in items)} for phase, items in groups.items()}
    (root / 'summary.json').write_text(json.dumps(summary, indent=2))
    limitations = (['Display cache retains the previous frame after all project tracks are removed.']
                   if backend == 'display' else ['grabVideoFrame returns no frames with the tested monitoring video effect.'])
    (root / 'limitations.json').write_text(json.dumps(limitations, indent=2))
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('profile', 'reaper', 'probe', 'media'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--resource-source', type=Path, help='Existing resource directory for license and audio settings')
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--backend', choices=('grab', 'display'), default='display')
    args = parser.parse_args()
    key = (sys.platform, platform.machine())
    assert key in BUILDS, f'Unsupported platform: {key}'
    filename, expected = BUILDS[key]
    executable = args.reaper.resolve()
    plugin_dir = executable.parent.parent / 'Plugins' if sys.platform == 'darwin' else executable.parent / 'Plugins'
    module = plugin_dir / filename
    assert hashlib.sha256(module.read_bytes()).hexdigest() == expected, 'Unsupported video module build'
    assert args.probe.is_file() and args.media.is_file()
    root = args.profile.resolve()
    root.mkdir(parents=True, exist_ok=False)
    (root / 'UserPlugins').mkdir()
    (root / 'Empty').mkdir()
    suffix = {'win32': '.dll', 'darwin': '.dylib', 'linux': '.so'}[sys.platform]
    shutil.copy2(args.probe, root / 'UserPlugins' / ('reaper_video_internals_probe' + suffix))
    source = args.resource_source or executable.parent
    if sys.platform == 'darwin' and not args.resource_source:
        source = Path.home() / 'Library/Application Support/REAPER'
    config = ('[reaper]\nloadlastproj=0\nshowlastproj=0\nsplash=0\nverchk=0\nerrnowarn=5\n'
              'hasrecentlyopened=1\nnewprojdo=0\nvst_scan=0\nvstfullstate=58181\nvstpath=\nvstpath64=\n'
              'clap_path=' + str(root / 'Empty') + '\n')
    if sys.platform == 'win32':
        shutil.copy2(executable, root / 'reaper.exe')
        executable = root / 'reaper.exe'
        (root / 'Plugins').mkdir()
        for name in (filename, 'reaper_wave.dll', 'FFmpeg.dll'):
            shutil.copy2(plugin_dir / name, root / 'Plugins' / name)
        config += ('clap_path_win64=' + str(root / 'Empty') + '\nlv2path_win64=;\n[audioconfig]\nmode=4\n'
                   'waveout_devicein=-1\nwaveout_deviceout=0\nwaveout_srate=48000\nwaveout_bps=16\n'
                   'waveout_bs=256\nwaveout_numblocks=4\nwaveout_nch_out=2\n')
    elif sys.platform == 'darwin':
        previous = configparser.RawConfigParser(strict=False)
        previous.read(source / 'reaper.ini')
        if previous.has_section('reaper'):
            config += ''.join(k + '=' + v + '\n' for k, v in previous['reaper'].items() if k.startswith('coreaudio'))
    else:
        config += 'linux_audio_mode=3\nlinux_audio_srate=48000\nlinux_audio_bsize=512\nlinux_audio_nch_out=2\n'
    config += '[reaper_video]\nvisible=0\n'
    (root / 'reaper.ini').write_text(config)
    for name in ('reaper-license.rk', 'reaper-install-rev.txt'):
        if (source / name).exists():
            shutil.copy2(source / name, root / name)
    png(root / 'red.png', (210, 30, 60))
    png(root / 'blue.png', (30, 80, 220))
    shutil.copy2(args.media, root / 'motion.mp4')
    runner = Path(__file__).with_name('video_internal_runner.lua').read_text()
    (root / 'runner.lua').write_text(runner.replace('__BACKEND__', args.backend))
    env = os.environ.copy()
    env['REAWEB_VIDEO_INTERNAL_PROBE'] = '1'
    env['CLAP_PATH'] = str(root / 'Empty')
    options = {}
    if sys.platform == 'win32':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        options['startupinfo'] = startup
    with (root / 'process.log').open('w') as log:
        process = subprocess.Popen([str(executable), '-newinst', '-cfgfile', str(root / 'reaper.ini'),
                                    '-new', '-ignoreerrors', '-nosplash', str(root / 'runner.lua')],
                                   cwd=root, env=env, stdout=log, stderr=log, **options)
        try:
            for _ in range(args.timeout * 2):
                if process.poll() is not None:
                    break
                if (root / 'results.jsonl').exists():
                    results = (root / 'results.jsonl').read_text()
                    if '"done":true' in results or '"error":' in results:
                        break
                time.sleep(.5)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    print(json.dumps(validate(root, args.backend), indent=2))


if __name__ == '__main__':
    main()
