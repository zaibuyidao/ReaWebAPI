"""Opt-in WebView2 latency probe; uses the mock host, never the user's REAPER."""
import ctypes as C
from ctypes import wintypes as W
import json
import time


def run_windows(host):
    folder = host['root'] / 'Scripts' / 'latency'
    folder.mkdir(parents=True, exist_ok=True)
    page = folder / 'index.html'
    page.write_text('<!doctype html><meta charset="utf-8"><pre id="output">Connecting...</pre>'
                    '<script src="app.js"></script>', encoding='utf-8')
    (folder / 'app.js').write_text('''(async () => {
  const started = performance.now();
  const capabilities = await reaper.lifecycle.ready;
  const readyMs = performance.now() - started;
  await reaper.window.setTitle('Latency probe');
  const track = await reaper.GetSelectedTrack(0, 0);
  if (!track) throw new Error('Latency probe needs the mock track');
  const [ok, name] = await reaper.GetTrackName(track);
  if (!ok) throw new Error('Track name query failed');
  document.getElementById('output').textContent = name;
  const nameMs = performance.now() - started;
  const samples = [];
  for (let i = 0; i < 30; ++i) {
    const start = performance.now();
    const [success, value] = await reaper.GetTrackName(track);
    if (!success || value !== name) throw new Error('Incorrect track response');
    samples.push(performance.now() - start);
  }
  samples.sort((a, b) => a - b);
  await reaper.debug.log('LATENCY ' + JSON.stringify({ readyMs, nameMs,
    callMedianMs: samples[15], callP95Ms: samples[28],
    capabilitiesBytes: new TextEncoder().encode(JSON.stringify(capabilities)).length }));
  await reaper.window.close();
})().catch(error => reaper.debug.error('LATENCY_ERROR ' + String(error)));
''', encoding='utf-8')
    user32 = C.windll.user32
    user32.PeekMessageW.argtypes = [C.POINTER(W.MSG), W.HWND, W.UINT, W.UINT, W.UINT]
    user32.TranslateMessage.argtypes = [C.POINTER(W.MSG)]
    user32.DispatchMessageW.argtypes = [C.POINTER(W.MSG)]
    for attempt in range(2):
        offset = len(host['messages'])
        started = time.monotonic()
        identifier = host['open_window'](str(page).encode('utf-8'))
        assert identifier, host['get_error']()
        next_tick = started
        while host['is_open'](identifier):
            assert time.monotonic() - started < 30, host['messages'][offset:]
            message = W.MSG()
            for _ in range(100):
                if not user32.PeekMessageW(C.byref(message), None, 0, 0, 1):
                    break
                user32.TranslateMessage(C.byref(message))
                user32.DispatchMessageW(C.byref(message))
            now = time.monotonic()
            if now >= next_tick:
                host['timer']()
                next_tick = now + 0.03
            time.sleep(0.001)
        host['timer']()
        lines = host['messages'][offset:]
        assert not any('LATENCY_ERROR' in line for line in lines), lines
        records = [json.loads(line.split('LATENCY ', 1)[1]) for line in lines if 'LATENCY ' in line]
        assert len(records) == 1, lines
        print(json.dumps({'attempt': attempt + 1, 'mockHostTickMs': 30, **records[0]}, ensure_ascii=False))
