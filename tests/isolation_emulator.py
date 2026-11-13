"""Opt-in acceptance against a supplied EmulatorJS distribution and legal fixtures."""
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import shutil
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from functools import partial
import threading
import os
import base64
import hashlib
from isolation_fixtures import dos_program, nes_program


def run_windows(host):
    source = host['args'].isolation_emulator.resolve()
    app = host['root'] / 'Scripts/Emulator'
    app.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source / 'vendor/emulatorjs', app / 'vendor/emulatorjs', dirs_exist_ok=True)
    psp_program = host['args'].isolation_psp_program or source / 'cube.elf'
    shutil.copyfile(psp_program, app / psp_program.name)
    shutil.copyfile(Path(__file__).with_suffix('.js'), app / 'test.js')
    (app / 'app.json').write_text(json.dumps({'id': 'emulator-isolation-test', 'webRuntime': {'crossOriginIsolation': 'required'}}))
    (app / 'test.com').write_bytes(dos_program())
    (app / 'palette.nes').write_bytes(nes_program())
    user32 = host['user32']
    user32.PeekMessageW.argtypes = [C.POINTER(W.MSG), W.HWND, W.UINT, W.UINT, W.UINT]
    user32.TranslateMessage.argtypes = [C.POINTER(W.MSG)]
    user32.DispatchMessageW.argtypes = [C.POINTER(W.MSG)]
    user32.SendMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
    user32.SendMessageW.restype = C.c_ssize_t

    def keyboard_message(key, pressed):
        handles = []
        callback_type = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
        def child_callback(hwnd, _):
            name = C.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, name, len(name))
            if name.value == 'Chrome_RenderWidgetHostHWND': handles.append(hwnd)
            return True
        child_callback = callback_type(child_callback)
        def top_callback(hwnd, _):
            pid = W.DWORD()
            user32.GetWindowThreadProcessId(hwnd, C.byref(pid))
            if pid.value == os.getpid(): user32.EnumChildWindows(hwnd, child_callback, 0)
            return True
        user32.EnumWindows(callback_type(top_callback), 0)
        assert handles, 'Test WebView keyboard target was not found'
        scan = user32.MapVirtualKeyW(key, 0)
        for hwnd in set(handles):
            user32.SendMessageW(hwnd, 0x100 if pressed else 0x101, key,
                               1 | (scan << 16) | (0 if pressed else 0xc0000000))
    reports = []
    server = None
    if host['args'].isolation_emulator_http:
        class IsolatedHandler(SimpleHTTPRequestHandler):
            def end_headers(self):
                self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
                self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
                super().end_headers()
            def log_message(self, *_): pass
        server = ThreadingHTTPServer(('127.0.0.1', 0), partial(IsolatedHandler, directory=str(app)))
        threading.Thread(target=server.serve_forever, daemon=True).start()
    cases = [('ppsspp', psp_program.name, True), ('dosbox_pure', 'test.com', True), ('fceumm', 'palette.nes', False), ('fceumm', 'palette.nes', True)]
    if host['args'].isolation_core:
        cases = [case for case in cases if case[0] == host['args'].isolation_core]
    cases *= 1 + host['args'].isolation_reopen
    for cycle, (core, game, threads) in enumerate(cases):
        soak_seconds = host['args'].isolation_soak_seconds if cycle == 0 else 0
        options = {}
        if core == 'ppsspp':
            if host['args'].isolation_psp_interpreter:
                options.update(ppsspp_cpu_core='Interpreter', ppsspp_fast_memory='disabled')
            if host['args'].isolation_psp_model:
                options['ppsspp_psp_model'] = host['args'].isolation_psp_model
        (app / 'index.html').write_text('''<!doctype html><meta charset="utf-8"><title>Core acceptance</title>
        <div id="game" style="width:640px;height:480px"></div><script>''' +
            'window.EJS_player="#game";window.EJS_core=' + json.dumps(core) + ';window.EJS_gameUrl=' + json.dumps(game) + ';' +
            'window.EJS_pathtodata="vendor/emulatorjs/data/";window.EJS_threads=' + json.dumps(threads) + ';' +
            'window.EJS_startOnLoaded=true;window.EJS_DEBUG_XX=true;window.EJS_disableAutoLang=true;window.EJS_language="en-US";' +
            'window.EJS_disableLocalStorage=true;' +
            'window.acceptancePspScenario=' + json.dumps(host['args'].isolation_psp_scenario) + ';' +
            'window.acceptanceSoakSeconds=' + str(soak_seconds) + ';' +
            'window.EJS_defaultOptions=' + json.dumps(options) + ';' +
            '</script><script src="test.js"></script><script src="vendor/emulatorjs/data/loader.js"></script>')
        host['messages'].clear()
        opened = time.monotonic()
        identifier = host['dev_open'](f'http://127.0.0.1:{server.server_port}/index.html'.encode()) if server else host['open_window'](str(app / 'index.html').encode())
        assert identifier, host['get_error']()
        deadline = time.monotonic() + 100 + soak_seconds
        message_index = 0
        while time.monotonic() < deadline and not any(m.startswith('CORE_REPORT:') for m in host['messages']):
            msg = W.MSG()
            for _ in range(100):
                if not user32.PeekMessageW(C.byref(msg), None, 0, 0, 1): break
                user32.TranslateMessage(C.byref(msg)); user32.DispatchMessageW(C.byref(msg))
            host['timer'](); time.sleep(.005)
            for message in host['messages'][message_index:]:
                if message.startswith('CORE_KEY:'):
                    key, pressed = map(int, message.split(':', 1)[1].split(','))
                    keyboard_message(key, pressed)
                elif message.startswith('CORE_IMAGE:'):
                    _, label, data = message.strip().split(':', 2)
                    assert label in ('psp-pressed', 'psp-alternate', 'psp-restored')
                    (host['root'] / (label + '.png')).write_bytes(base64.b64decode(data, validate=True))
            message_index = len(host['messages'])
        matches = [m for m in host['messages'] if m.startswith('CORE_REPORT:')]
        report = json.loads(matches[-1].split(':', 1)[1]) if matches else {'core': core, 'passed': False, 'error': 'No report'}
        report['cycle'] = cycle
        archive = app / 'vendor/emulatorjs/data/cores' / (core + ('-thread' if threads else '') + '-wasm.data')
        report['coreArchiveSha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
        report['programSha256'] = hashlib.sha256((app / game).read_bytes()).hexdigest()
        report['coreOptions'] = options
        report['openAndTestSeconds'] = time.monotonic() - opened
        reports.append(report)
        closing = time.perf_counter()
        host['close_window'](identifier)
        report['closeCallSeconds'] = time.perf_counter() - closing
        report['closeClockResolutionSeconds'] = time.get_clock_info('perf_counter').resolution
        for _ in range(200 if host['args'].isolation_reopen else 40):
            msg = W.MSG()
            for _ in range(100):
                if not user32.PeekMessageW(C.byref(msg), None, 0, 0, 1): break
                user32.TranslateMessage(C.byref(msg)); user32.DispatchMessageW(C.byref(msg))
            host['timer'](); time.sleep(.01)
        (host['root'] / 'emulator-report.json').write_text(json.dumps(reports, indent=2))
    if server: server.shutdown(); server.server_close()
    assert all(r['passed'] for r in reports), [(r['core'], r.get('error'), r.get('logs')) for r in reports]
    print('PASS:', [(report['core'], report.get('threads'), report['cycle']) for report in reports])
