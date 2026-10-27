"""Opt-in real REAPER Lua/EEL2 API regression in a fresh isolated profile."""
import argparse
import configparser
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import sys
import time
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread

ROOT = Path(__file__).resolve().parents[1]
PAGE = '''<!doctype html><meta charset="utf-8"><title>EEL API compatibility</title>
<h1>EEL / Lua API compatibility probe</h1><script>
(async () => {
  let last;
  await reaper.events.on('message', text => reaper.host.send('echo:' + text));
  await reaper.events.on('frame', frame => {
    if (typeof frame.lua === 'string' && frame.lua !== last) {
      last = frame.lua; reaper.host.send('frame:' + frame.lua);
    }
  });
  await reaper.host.send('ready');
})();
</script>'''


def prepare(directory, executable, extension):
    directory.mkdir(parents=True, exist_ok=False)
    (directory / 'UserPlugins').mkdir()
    (directory / 'Plugins').mkdir()
    shutil.copy2(extension, directory / 'UserPlugins' / extension.name)
    if sys.platform == 'darwin':
        subprocess.run(['codesign', '--force', '--sign', '-', str(directory / 'UserPlugins' / extension.name)], check=True)
    if sys.platform.startswith('linux'):
        helper = extension.parent / extension.name.replace('reaper_reawebapi-', 'reawebapi-webview-').removesuffix('.so')
        shutil.copy2(helper, directory / 'UserPlugins' / helper.name)
        (directory / 'UserPlugins' / helper.name).chmod(0o755)
    if sys.platform == 'win32':
        shutil.copy2(executable, directory / 'reaper.exe')
    (directory / 'executable.txt').write_text(str(directory / 'reaper.exe' if sys.platform == 'win32' else executable), encoding='utf-8')
    for name in ('jsfx.dll', 'reaper_wave.dll', 'reaper_midi.dll'):
        source = executable.parent / 'Plugins' / name
        if source.exists():
            shutil.copy2(source, directory / 'Plugins' / name)
    resource = Path.home() / 'Library/Application Support/REAPER' if sys.platform == 'darwin' else executable.parent
    for name in ('reaper-license.rk', 'reaper-install-rev.txt'):
        if (resource / name).exists():
            shutil.copy2(resource / name, directory / name)
    settings = '[reaper]\nhasrecentlyopened=1\nloadlastproj=0\nnewprojdo=0\nerrnowarn=5\nverchk=0\nvstpath64=\nvstfullstate=58181\nlv2path_win64=;\nclap_path_win64=\n'
    if sys.platform == 'darwin':
        user = configparser.RawConfigParser(strict=False)
        user.read(resource / 'reaper.ini')
        if user.has_section('reaper'):
            settings += ''.join(f'{k}={v}\n' for k,v in user['reaper'].items() if k.startswith('coreaudio'))
    elif sys.platform.startswith('linux'):
        settings += 'linux_audio_mode=3\nlinux_audio_srate=48000\nlinux_audio_bsize=512\nlinux_audio_nch_out=2\n'
    else:
        settings += '[audioconfig]\nmode=0\n'
    (directory / 'reaper.ini').write_text(settings, encoding='utf-8')
    (directory / 'app').mkdir()
    (directory / 'app' / 'app.json').write_text(
        '{"id":"eel-api-probe","name":"EEL API probe","version":"1.0.0","entry":"index.html"}', encoding='utf-8')
    (directory / 'app' / 'index.html').write_text(PAGE, encoding='utf-8')


def launch(directory, script):
    startup = None
    if sys.platform == 'win32':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
    executable = (directory / 'executable.txt').read_text(encoding='utf-8')
    process = subprocess.Popen([executable, '-newinst', '-cfgfile',
                                str(directory / 'reaper.ini'), '-new', '-ignoreerrors', '-nosplash', str(script)],
                               cwd=directory, startupinfo=startup)
    (directory / 'pid.txt').write_text(str(process.pid))
    return process


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reaper', type=Path, required=True)
    parser.add_argument('--extension', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--suite', choices=('strings', 'full'), required=True)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--direct', action='store_true', help='Use EEL direct extension calls (REAPER 6.24+)')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    metadata = {'extensionSha256': hashlib.sha256(args.extension.read_bytes()).hexdigest(),
                'suite': args.suite,
                'syntax': 'direct' if args.direct or args.suite == 'strings' else 'extension_api',
                'cases': {}}
    running = {}
    server = None
    try:
        if args.suite == 'strings':
            for language in ('eel','lua'):
                directory=output/language
                prepare(directory,args.reaper.resolve(),args.extension.resolve())
                (directory/'app/index.html').write_text('''<!doctype html><title>String size probe</title><script>
(async()=>{
await reaper.events.on('message',text=>reaper.host.send(text.startsWith('bytes:')?'x'.repeat(Number(text.slice(6))):'length:'+text.length));
await reaper.host.send('ready');
})();</script>''',encoding='utf-8')
                script=directory/f'eel_api_strings.{language}'
                shutil.copy2(ROOT/'tests'/script.name,script)
                running[language]=(directory,launch(directory,script))
        else:
            directory = output / 'full'
            prepare(directory, args.reaper.resolve(), args.extension.resolve())
            server = ThreadingHTTPServer(('127.0.0.1', 0), partial(SimpleHTTPRequestHandler, directory=str(directory)))
            Thread(target=server.serve_forever, daemon=True).start()
            (directory / 'dev-url.txt').write_text(f'http://127.0.0.1:{server.server_port}/app/index.html', encoding='utf-8')
            for name in ('eel_api_compatibility.eel', 'eel_api_compatibility.lua'):
                shutil.copy2(ROOT / 'tests' / name, directory / name)
            if args.direct:
                script = directory / 'eel_api_compatibility.eel'
                content = re.sub(r'extension_api\("(ReaWeb_\w+)",\s*', r'\1(', script.read_text(encoding='utf-8'))
                script.write_text(content, encoding='utf-8')
            running['full'] = (directory, launch(directory, directory / 'eel_api_compatibility.lua'))
        deadline = time.monotonic() + args.timeout
        while running and time.monotonic() < deadline:
            for name, (directory, process) in list(running.items()):
                result_path = directory / 'result.txt'
                result = result_path.read_text(encoding='utf-8') if result_path.exists() else ''
                observed = directory / 'observed-error.txt'
                error = observed.read_text(encoding='utf-8') if observed.exists() else ''
                done = error or 'COMPLETE' in result or process.poll() is not None
                if done:
                    code = process.poll()
                    metadata['cases'][name] = {'result': result, 'observedError': error, 'exitCodeBeforeCleanup': code,
                                              'exitCodeHex': None if code is None else f'0x{code & 0xffffffff:08X}'}
                    print(name + ': ' + (error or result), flush=True)
                    stop(process)
                    del running[name]
            time.sleep(.2)
        for name in running:
            metadata['cases'][name] = {'result': 'INCONCLUSIVE: timeout; inspect REAPER UI'}
    finally:
        for _, process in running.values():
            stop(process)
        if server:
            server.shutdown()
            server.server_close()
        (output / 'summary.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding='utf-8')
    if any('INCONCLUSIVE' in c['result'] for c in metadata['cases'].values()):
        raise SystemExit(1)
    if args.suite == 'strings':
        for case in metadata['cases'].values():
            assert 'COMPLETE' in case['result'] and not case.get('observedError')
            inbound = set()
            for line in case['result'].splitlines():
                fields = line.split('\t')
                if len(fields) != 3:
                    continue
                kind, wanted, actual = fields[0], int(fields[1]), int(fields[2])
                if kind in ('BUILD','OUTBOUND','INBOUND','OUTBOUND_RECEIVED_STRING'):
                    assert wanted == actual, line
                elif kind in ('SEND_ACCEPTED','SEND_RECEIVED_STRING'):
                    assert actual == 1, line
                if kind == 'INBOUND':
                    inbound.add(wanted)
            assert inbound == {1024,16384,65536,1048576}
    if args.suite == 'full':
        for language in ('eel', 'lua'):
            result = (output / 'full' / f'{language}.tsv').read_text(encoding='utf-8')
            print(result)
            if 'FAIL\t' in result or 'COMPLETE\t0 failures' not in result:
                raise SystemExit(1)
            appearance = json.loads((output / 'full' / f'{language}-appearance.json').read_text(encoding='utf-8'))
            assert appearance['window']['frameless'] is True
            assert appearance['window']['resizable'] is False
            assert appearance['window']['transparent'] is True
            hidden = json.loads((output / 'full' / f'{language}-hidden.json').read_text(encoding='utf-8'))
            shown = json.loads((output / 'full' / f'{language}-shown.json').read_text(encoding='utf-8'))
            assert hidden['window']['visible'] is False
            assert shown['window']['visible'] is True


if __name__ == '__main__':
    main()
