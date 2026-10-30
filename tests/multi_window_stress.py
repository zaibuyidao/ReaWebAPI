"""Opt-in multi-window traffic and official Demo acceptance in isolated REAPER."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import time

from eel_api_compatibility import prepare, launch, stop

ROOT = Path(__file__).resolve().parents[1]
PAGE = '''<!doctype html><meta charset="utf-8"><title>Lua traffic</title>
<style>body{background:#192331;color:#ddd;font:16px sans-serif}pre{white-space:pre-wrap}</style>
<h2>Lua backend stress</h2><small id="frame"></small><pre id="state">Starting</pre><script>
(async () => {
  await reaper.events.on('message', async text => {
    document.querySelector('#state').textContent = text.slice(0,100);
    await reaper.host.send(text);
  });
  await reaper.events.on('frame', frame => { document.querySelector('#frame').textContent = 'Frame ' + frame.sequence; });
  await reaper.host.send('ready');
})();</script>'''
DEMO_PROBE = '''
let stressBusy = false, stressCount = 0;
setInterval(async () => {
  if (stressBusy || !connected || pageClosed) return;
  stressBusy = true;
  try {
    await request(stressCount % 2 && backendState?.track ? 'fx' : 'project');
    await reaper.SetExtState('MultiWindowStress', 'demo', String(++stressCount), false);
  } catch (error) {
    await reaper.SetExtState('MultiWindowStress', 'demo', 'FAIL:' + error.message, false);
  } finally { stressBusy = false; }
}, 200);
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reaper', type=Path, required=True)
    parser.add_argument('--extension', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--counts', default='8,16,32')
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--heavy-only', action='store_true')
    args = parser.parse_args()
    counts = [int(value) for value in args.counts.split(',')]
    if not counts or min(counts) < 2 or max(counts) > 32 or args.seconds < 4:
        parser.error('counts include the Demo and must be 2..32, seconds at least 4')
    root = args.output.resolve()
    prepare(root, args.reaper.resolve(), args.extension.resolve())
    if sys.platform == 'win32':
        config = root / 'reaper.ini'
        config.write_text(config.read_text().replace('[reaper]', '[reaper]\naudiomode=4').replace(
            '[audioconfig]\nmode=0', '[audioconfig]\nmode=4\nwaveout_devicein=-1\nwaveout_deviceout=0\nwaveout_srate=48000\nwaveout_bps=16\nwaveout_bs=256\nwaveout_numblocks=4\nwaveout_nch_out=2'))
    (root / 'app/index.html').write_text(PAGE, encoding='utf-8')
    (root / 'app/app.json').write_text('{"id":"multi-window-stress","width":360,"height":240}')
    demo = root / 'demo'
    demo.mkdir()
    for path in (ROOT / 'web').iterdir():
        if path.is_file():
            shutil.copy2(path, demo / path.name)
    with (demo / 'app.js').open('a', encoding='utf-8') as stream:
        stream.write(DEMO_PROBE)
    effects = root / 'Effects'
    effects.mkdir(exist_ok=True)
    (effects / 'stress-load').write_text('''desc:Multi-window test load
options:gmem=MultiWindowStress
@init
phase=0;
@block
gmem[0]+=1;
@sample
phase+=0.001;
v=phase;
loop(16,v=sin(v)+0.1);
spl0=sin(phase)*0.00001;
spl1=spl0;
''')
    script = (ROOT / 'tests/multi_window_stress.lua').read_text(encoding='utf-8')
    script = script.replace('STRESS_COUNTS', ','.join(map(str, counts))).replace('STRESS_SECONDS', str(args.seconds))
    script = script.replace('STRESS_HEAVY', 'true' if args.heavy_only else 'false')
    (root / 'runner.lua').write_text(script, encoding='utf-8')
    (root / 'metadata.json').write_text(json.dumps({
        'extensionSha256': hashlib.sha256(args.extension.read_bytes()).hexdigest(),
        'counts': counts, 'secondsPerCase': args.seconds,
        'heavyOnly': args.heavy_only,
        'load': '50 tracks, 80 JSFX instances, playback, frame subscriptions, official Demo',
    }, indent=2), encoding='utf-8')
    process = launch(root, root / 'runner.lua')
    try:
        deadline = time.monotonic() + sum(counts) * 6 + len(counts) * (args.seconds + 35) * 2
        while not (root / 'result.txt').exists() and process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.25)
        result = (root / 'result.txt').read_text(encoding='utf-8') if (root / 'result.txt').exists() else 'FAIL: no result'
    finally:
        stop(process)
    print(result, flush=True)
    return 0 if result.startswith('PASS:') else 1


if __name__ == '__main__':
    raise SystemExit(main())
