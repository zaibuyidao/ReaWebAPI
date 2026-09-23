"""Opt-in title checks in WebView2 and native Windows captions, using the mock host."""
import ctypes as C
from ctypes import wintypes as W
import json
import time


SCRIPT = r'''
(async () => {
  await reaper.lifecycle.ready;
  const pageTitle = document.title;
  const initial = pageTitle || 'ReaWebAPI — titles';
  const phase = sessionStorage.getItem('titlePhase');
  let resume;
  await reaper.events.on('message', () => { if (resume) { resume(); resume = null; } });
  const delay = () => new Promise(resolve => setTimeout(resolve, 20));
  const check = async expected => {
    for (let i = 0; (await reaper.window.getState()).title !== expected; ++i) {
      if (i > 150) throw new Error('Title did not become ' + expected);
      await delay();
    }
    const acknowledged = new Promise(resolve => { resume = resolve; });
    await reaper.host.send(JSON.stringify({ expected }));
    await acknowledged;
  };
  if (phase === 'explicit') {
    await check('Explicit 标题');
    if (document.title !== pageTitle) throw new Error('Native override changed document.title');
    sessionStorage.removeItem('titlePhase');
    await reaper.window.close();
    return;
  }
  await check(initial);
  if (!phase) {
    document.title = '我的工具 🎵'; await check('我的工具 🎵');
    document.querySelector('title').firstChild.data = 'Text node title'; await check('Text node title');
    document.querySelector('title').remove(); await check('ReaWebAPI — titles');
    document.title = '   '; await check('ReaWebAPI — titles');
    document.head.innerHTML = '<title>Replacement head title</title>'; await check('Replacement head title');
    document.title = 'a'.repeat(255) + '🎵'; await check('a'.repeat(255));
    await reaper.window.setDocked(true);
    document.title = 'Docked 标题'; await check('Docked 标题');
    await reaper.window.setDocked(false); await check('Docked 标题');
    sessionStorage.setItem('titlePhase', 'automatic');
    await reaper.window.reload();
    return;
  }
  await reaper.window.setTitle('Explicit 标题');
  document.title = 'Ignored page title'; await delay();
  await check('Explicit 标题');
  sessionStorage.setItem('titlePhase', 'explicit');
  await reaper.window.reload();
})().catch(error => reaper.host.send(JSON.stringify({ error: String(error) })));
'''


def run_windows(host):
    folder = host['root'] / 'Scripts' / 'titles'
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'app.js').write_text(SCRIPT, encoding='utf-8')
    user32 = host['user32']
    user32.PeekMessageW.argtypes = [C.POINTER(W.MSG), W.HWND, W.UINT, W.UINT, W.UINT]
    user32.TranslateMessage.argtypes = [C.POINTER(W.MSG)]
    user32.DispatchMessageW.argtypes = [C.POINTER(W.MSG)]
    user32.GetWindowTextW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
    user32.GetClassNameW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
    callback_type = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    user32.EnumThreadWindows.argtypes = [W.DWORD, callback_type, W.LPARAM]

    def pump():
        message = W.MSG()
        for _ in range(100):
            if not user32.PeekMessageW(C.byref(message), None, 0, 0, 1):
                break
            user32.TranslateMessage(C.byref(message))
            user32.DispatchMessageW(C.byref(message))
        host['timer']()
        time.sleep(.01)

    for declared in ('SendFlow', ''):
        page = folder / 'index.html'
        page.write_text('<!doctype html><meta charset="utf-8">' +
                        (f'<title>{declared}</title>' if declared else '') +
                        '<script src="app.js" defer></script>', encoding='utf-8')
        identifier = host['open_window'](str(page).encode('utf-8'), b'@private/title-launcher.lua')
        assert identifier, host['get_error']()
        handles = []

        @callback_type
        def find_window(handle, _):
            name = C.create_unicode_buffer(128)
            user32.GetClassNameW(handle, name, 128)
            if name.value == 'ReaWebAPI.Window':
                handles.append(handle)
            return True

        user32.EnumThreadWindows(C.windll.kernel32.GetCurrentThreadId(), find_window, 0)
        assert len(handles) == 1, handles
        checked = []
        deadline = time.monotonic() + 40
        while host['is_open'](identifier):
            assert time.monotonic() < deadline, checked
            pump()
            if not host['is_open'](identifier):
                break
            text = host['host_receive'](identifier)
            if not text:
                continue
            report = json.loads(text)
            assert 'error' not in report, report
            caption = C.create_unicode_buffer(512)
            user32.GetWindowTextW(handles[0], caption, 512)
            assert caption.value == report['expected'], (caption.value, report)
            assert host['open_window'](b'missing.html', b'@private/title-launcher.lua') == identifier
            checked.append(caption.value)
            assert host['host_send'](identifier, b'next')
        host['timer']()
        initial = declared or 'ReaWebAPI — titles'
        assert checked == [initial, '我的工具 🎵', 'Text node title', 'ReaWebAPI — titles',
                           'ReaWebAPI — titles', 'Replacement head title', 'a' * 255,
                           'Docked 标题', 'Docked 标题', initial, 'Explicit 标题', 'Explicit 标题'], checked
    assert not host['docked'] and not host['dock_failures'], host['dock_failures']
    print('WebView2 titles: HTML, dynamic/Unicode titles, DOM replacement/removal, empty fallback, '
          'UTF-8 boundary, docking, explicit priority, reload and instance reuse passed (24 native captions)')
