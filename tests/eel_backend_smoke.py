"""Opt-in real EEL2/WebView test in a NEW isolated REAPER resource directory.

Lua only creates the disposable project and observes test results. The shipped
EEL2 launcher is unmodified; button requests and replies pass through EEL2.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
from eel_api_compatibility import prepare

PROBE = r'''
(async () => {
  const expected = '鼓组 "A" \\ <b>🎛</b>\n\tend';
  const pause = () => new Promise(resolve => setTimeout(resolve, 50));
  const wait = async predicate => { for (let i = 0; i < 300; i++) { if (await predicate()) return; await pause(); } throw new Error('page timeout'); };
  const mark = text => reaper.SetExtState('EEL.Validation', 'stage', text, false);
  let framesSeen=0, messageStates=0, supersededSeen=false;
  try {
    const diagnostic = setInterval(() => {
      void reaper.SetExtState('EEL.Validation', 'progress', JSON.stringify({framesSeen,
        name: document.querySelector('#name').textContent, status: document.querySelector('#status').textContent,
        error: document.querySelector('#error').textContent, volume: document.querySelector('#volume').value}), false);
    }, 1000);
    window.addEventListener('pagehide', () => clearInterval(diagnostic));
    await reaper.events.on('frame', frame => { if(frame.lua?.startsWith('state ')) framesSeen++; });
    await reaper.events.on('message', text => { if(text.startsWith('state ')) messageStates++; if(text.includes(' superseded ')) supersededSeen=true; });
    await wait(() => !document.querySelector('#refresh').disabled);
    document.querySelector('#refresh').click();
    await wait(() => document.querySelector('#name').textContent === expected);
    if (document.querySelector('#name').children.length) throw new Error('track name interpreted as HTML');
    if (await reaper.GetExtState('EEL.Validation', 'next') === 'reopen') { await mark('reopened'); return; }
    if (sessionStorage.reloading) { await mark('reloaded'); return; }
    const volume = document.querySelector('#volume');
    const track = await reaper.GetSelectedTrack(0,0);
    const set = value => { volume.value=value; volume.dispatchEvent(new Event('input')); volume.dispatchEvent(new Event('change')); };
    await wait(() => !document.querySelector('#controls').disabled && framesSeen > 0);
    set(0); await drainSlider('volume');
    if(await reaper.GetMediaTrackInfo_Value(track,'D_VOL')!==0) throw Error('Zero gain failed');
    for(const value of [-.1,4.1]) {
      let invalid=false;
      try { await request('volume',value); } catch(error) { invalid=error.message.includes('invalid'); }
      if(!invalid || await reaper.GetMediaTrackInfo_Value(track,'D_VOL')!==0) throw Error('Out-of-range gain accepted');
    }
    const old = `state ${session} ${state.revision} ${state.context} ${state.guid} 0\nold snapshot`;
    for(let i=0;i<500;i++) set(i/125);
    set(4);
    if(sliders.volume.flights.size!==8 || sliders.volume.queued.value!==4) throw Error('Unbounded writes or lost final value');
    onFrame({lua:old}); await new Promise(requestAnimationFrame);
    if(volume.value!=='4') throw Error('Old frame overwrote pending value');
    await drainSlider('volume');
    onFrame({lua:old}); await new Promise(requestAnimationFrame);
    if(volume.value!=='4' || await reaper.GetMediaTrackInfo_Value(track,'D_VOL')!==4) throw Error('Final value not confirmed');
    if(!supersededSeen || !framesSeen || messageStates) throw Error('Incorrect acknowledgement or state channel');
    await mark('volume');
    await wait(() => volume.value==='3.5');
    await mark('selected');
    await wait(async () => await reaper.GetExtState('EEL.Validation', 'next') === 'none');
    document.querySelector('#refresh').click();
    await wait(() => document.querySelector('#name').textContent === 'No track selected');
    await mark('none');
    await wait(async () => await reaper.GetExtState('EEL.Validation', 'next') === 'reload');
    sessionStorage.reloading = 'yes';
    await reaper.window.reload();
  } catch (error) { await mark('FAIL: ' + error.message); }
})();
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reaper', type=Path, required=True)
    parser.add_argument('--extension', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--demo', choices=('eel-backend', 'eel-mixer'), default='eel-backend')
    args = parser.parse_args()
    root = args.output.resolve()
    prepare(root, args.reaper.resolve(), args.extension.resolve())
    executable = (root / 'executable.txt').read_text(encoding='utf-8')
    source = Path(__file__).resolve().parents[1]
    demo = root / '示例 空格' / args.demo
    shutil.copytree(source / 'web' / args.demo, demo)
    probe = PROBE if args.demo == 'eel-backend' else (source / 'tests/eel_mixer_smoke.js').read_text(encoding='utf-8')
    (demo / 'probe.js').write_text(probe, encoding='utf-8')
    page = demo / 'index.html'
    html = page.read_text(encoding='utf-8')
    if args.demo == 'eel-mixer':
        html = html.replace('<script src="app.js" defer>', '<script src="probe.js" defer></script>\n<script src="app.js" defer>')
    else:
        html += '<script src="probe.js" defer></script>\n'
    page.write_text(html, encoding='utf-8')
    runner = 'eel_backend_smoke.lua' if args.demo == 'eel-backend' else 'eel_mixer_smoke.lua'
    shutil.copy2(source / 'tests' / runner, root / 'runner.lua')
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
              for p in (source / 'web' / args.demo).iterdir() if p.is_file()}
    hashes['extension'] = hashlib.sha256(args.extension.read_bytes()).hexdigest()
    (root / 'inputs.json').write_text(json.dumps(hashes, indent=2), encoding='utf-8')
    command = [str(executable), '-newinst', '-cfgfile', str(root / 'reaper.ini'),
               '-new', '-ignoreerrors', '-nosplash', str(root / 'runner.lua')]
    startup = None
    if sys.platform == 'win32':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
    report = root / 'result.txt'
    with (root / 'process.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen(command, cwd=root, stdout=log, stderr=log, startupinfo=startup)
        try:
            deadline = time.monotonic() + 60
            result = ''
            while process.poll() is None and time.monotonic() < deadline:
                result = report.read_text(encoding='utf-8') if report.exists() else ''
                if 'PASS: complete' in result or 'FAIL:' in result:
                    break
                time.sleep(.2)
            print(result or 'FAIL: no result', flush=True)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    if 'PASS: complete' not in result:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
