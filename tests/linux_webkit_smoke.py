"""Exercise real WebKitGTK JS, shared profile and X11 reparenting, without touching REAPER."""
import ctypes as C
import json
import os
from pathlib import Path
import select
import signal
import socket
import time

root = Path(__file__).resolve().parents[1]
fixture = root / 'build-linux' / 'webkit-smoke'
fixture.mkdir(exist_ok=True)
profile = fixture / 'profile'
profile.mkdir(exist_ok=True)
page = fixture / 'index.html'
page.write_text('<!doctype html><meta charset="utf-8"><h1>WebKit smoke test</h1>')
x = C.CDLL('libX11.so.6')
x.XOpenDisplay.argtypes = [C.c_char_p]
x.XOpenDisplay.restype = C.c_void_p
x.XDefaultRootWindow.argtypes = [C.c_void_p]
x.XDefaultRootWindow.restype = C.c_ulong
x.XCreateSimpleWindow.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_uint, C.c_ulong, C.c_ulong]
x.XCreateSimpleWindow.restype = C.c_ulong
x.XMapWindow.argtypes = [C.c_void_p, C.c_ulong]
x.XDestroyWindow.argtypes = [C.c_void_p, C.c_ulong]
x.XSync.argtypes = [C.c_void_p, C.c_int]
x.XCloseDisplay.argtypes = [C.c_void_p]
x.XQueryTree.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong), C.POINTER(C.c_ulong), C.POINTER(C.POINTER(C.c_ulong)), C.POINTER(C.c_uint)]
x.XFree.argtypes = [C.c_void_p]
class WindowAttributes(C.Structure):
    _fields_ = [(n, C.c_int) for n in ('x', 'y', 'width', 'height', 'border_width', 'depth')] + [
        ('visual', C.c_void_p), ('root', C.c_ulong), ('window_class', C.c_int),
        ('bit_gravity', C.c_int), ('win_gravity', C.c_int), ('backing_store', C.c_int),
        ('backing_planes', C.c_ulong), ('backing_pixel', C.c_ulong), ('save_under', C.c_int),
        ('colormap', C.c_ulong), ('map_installed', C.c_int), ('map_state', C.c_int),
        ('all_event_masks', C.c_long), ('your_event_mask', C.c_long), ('do_not_propagate_mask', C.c_long),
        ('override_redirect', C.c_int), ('screen', C.c_void_p)]
x.XGetWindowAttributes.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(WindowAttributes)]
display = x.XOpenDisplay(None)
assert display, 'X11 display required'
parents = [x.XCreateSimpleWindow(display, x.XDefaultRootWindow(display), 30 + i*400, 30, 380, 300, 0, 0, 0) for i in range(2)]
for parent in parents: x.XMapWindow(display, parent)
x.XSync(display, 0)
parent, child = socket.socketpair()
helper = str(root / os.environ.get('REAWEB_TEST_BUILD_DIR', 'build-linux') / 'reawebapi-webview-x86_64')
env = dict(os.environ, GDK_BACKEND='x11')
# WSLg exposes X11. No sandbox switches are needed for the browser process.
process = os.posix_spawn(helper, [helper, str(profile)], env, file_actions=[(os.POSIX_SPAWN_DUP2, child.fileno(), 3), (os.POSIX_SPAWN_CLOSE, child.fileno())])
child.close()
parent.setblocking(False)
received = b''
results = []
idle_target = int(os.environ.get('REAWEB_TEST_IDLE_WEBVIEWS', '0'))
idle_state = None
filling = False

def send(message):
    parent.sendall((json.dumps(message) + '\n').encode())

def receive_until(condition, timeout=20):
    global received, idle_state
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not condition():
        if filling:
            send(dict(id=0, op='idle'))
        if not select.select([parent], [], [], .05)[0]: continue
        chunk = parent.recv(65536)
        assert chunk, 'WebKit exited early'
        received += chunk
        while b'\n' in received:
            line, received = received.split(b'\n', 1)
            message = json.loads(line)
            assert message['op'] != 'error', message
            if message['op'] == 'idle-pool':
                idle_state = message['state']
            if message['op'] == 'message':
                request = json.loads(message['message'])
                results.append((message['id'], request['method'], request.get('args')))
                value = '7.smoke'
                if request['method'] == '__reawebHello':
                    value = dict(protocol=1, projectEpoch=1, events=['windowstatechange'],
                                 methods=list(json.loads((root / 'api/bindings.json').read_text())['functions']))
                elif request['method'] in ('ReaWeb_GetWindowState', 'ReaWeb_Subscribe'):
                    value = dict(framelessActive=False)
                response = {'id': request['id'], 'document': request['document'], 'result': value}
                send({'id': message['id'], 'op': 'eval', 'script': 'window.__reawebReceive(' + json.dumps(response) + ');'})
            else: results.append((message.get('id', 0), message['op'], message.get('prewarmed') if message['op'] == 'container' else message.get('color')))
    assert condition(), [(row[0], row[1], str(row[2])[:200]) for row in results]

def geometry(id, target, focused=False):
    send(dict(id=id, op='geometry', parent=target, x=0, y=0, width=350, height=250, visible=True, focused=focused))

def children(window):
    root_return, parent_return, count = C.c_ulong(), C.c_ulong(), C.c_uint()
    pointer = C.POINTER(C.c_ulong)()
    assert x.XQueryTree(display, window, C.byref(root_return), C.byref(parent_return), C.byref(pointer), C.byref(count))
    result = [pointer[i] for i in range(count.value)]
    if pointer: x.XFree(pointer)
    return result

def mapped(window):
    attributes = WindowAttributes()
    assert x.XGetWindowAttributes(display, window, C.byref(attributes))
    return attributes.map_state == 2

try:
    receive_until(lambda: any(row[1] == 'ready' for row in results))
    if idle_target:
        send(dict(id=0, op='idle-pool', target=idle_target))
        filling = True
        receive_until(lambda: idle_state is not None and idle_state['ready'] == idle_target)
        filling = False
    bridge = '(() => {\n' + (root / 'runtime/reaper-api.generated.js').read_text() + (root / 'runtime/reaper.js').read_text() + '\n})();'
    script = bridge + '\nwindow.token="kept"; reaper.GetAppVersion().then(v => reaper.CountTracks(v === "7.smoke" ? 0 : 999));'
    send(dict(id=1, op='open', uri=page.as_uri(), script=script))
    geometry(1, parents[0])
    receive_until(lambda: (1, 'CountTracks', [0]) in results)
    if idle_target: assert (1, 'container', True) in results
    for background, expected in [('#182838', 0x182838), ('#e8d8c8', 0xe8d8c8), ('rgba(0,0,0,.5)', 0xffffff), ('#182838', 0x182838)]:
        results[:] = [row for row in results if row[1] != 'background']
        send(dict(id=1, op='eval', script=f'document.body.style.background={json.dumps(background)};'))
        receive_until(lambda: (1, 'background', expected) in results)
    for step in range(40):
        send(dict(id=1, op='geometry', parent=parents[0], x=0, y=0,
                  width=220 + step * 3, height=220 + step * 2, visible=True, focused=False))
    send(dict(id=1, op='eval', script='''(() => {
      const deadline = performance.now() + 5000;
      const check = () => {
        if (innerWidth === 337 && innerHeight === 298 && window.token === "kept") reaper.CountSelectedTracks(801);
        else if (performance.now() >= deadline) {
          reaper.debug.inspect({width: innerWidth, height: innerHeight, token: window.token});
          reaper.CountSelectedTracks(999);
        } else requestAnimationFrame(check);
      };
      requestAnimationFrame(check);
    })();'''))
    receive_until(lambda: (1, 'CountSelectedTracks', [801]) in results)
    geometry(1, parents[0])
    send(dict(id=1, op='eval', script='reaper.fs.writeFile("large.txt", "x".repeat(100000));'))
    receive_until(lambda: any(row[1] == 'ReaWeb_WriteFile' and len(row[2][1]) == 100000 for row in results))
    navigations = sum(row[1] == 'navigating' for row in results)
    send(dict(id=1, op='eval', script='location.hash="fragment"; reaper.CountSelectedTracks(42);'))
    receive_until(lambda: (1, 'CountSelectedTracks', [42]) in results)
    assert sum(row[1] == 'navigating' for row in results) == navigations, 'Fragment invalidated document'
    send(dict(id=1, op='focus'))
    send(dict(id=1, op='eval', script='let focusSeen=false; const focusProbe=setInterval(()=>{if(document.hasFocus()&&!focusSeen){focusSeen=true;reaper.CountSelectedTracks(701);}if(focusSeen&&!document.hasFocus()){clearInterval(focusProbe);reaper.CountSelectedTracks(702);}},20);'))
    receive_until(lambda: (1, 'CountSelectedTracks', [701]) in results)
    geometry(1, parents[0], focused=False)
    receive_until(lambda: (1, 'CountSelectedTracks', [702]) in results)
    first_children = children(parents[0])
    assert first_children, 'WebKit is not embedded'
    assert all(mapped(child) for child in first_children), 'WebKit native window is not visible in its foreign parent'
    send(dict(id=1, op='geometry', parent=parents[0], x=0, y=0, width=350, height=250, visible=False))
    send(dict(id=1, op='eval', script='reaper.CountSelectedTracks(703);'))
    receive_until(lambda: (1, 'CountSelectedTracks', [703]) in results)
    assert all(not mapped(child) for child in first_children), 'Hidden WebKit native window remained mapped'
    geometry(1, parents[0])
    send(dict(id=1, op='eval', script='reaper.CountSelectedTracks(704);'))
    receive_until(lambda: (1, 'CountSelectedTracks', [704]) in results)
    assert all(mapped(child) for child in first_children), 'WebKit native window did not remap'
    send(dict(id=1, op='park'))
    receive_until(lambda: (1, 'parked', None) in results)
    assert all(not mapped(child) for child in first_children), 'Parked WebKit native window remained visible'
    x.XDestroyWindow(display, parents[0])
    x.XSync(display, 0)
    parents[0] = x.XCreateSimpleWindow(display, x.XDefaultRootWindow(display), 30, 30, 380, 300, 0, 0, 0)
    x.XMapWindow(display, parents[0])
    x.XSync(display, 0)
    geometry(1, parents[1])
    send(dict(id=1, op='eval', script='reaper.CountSelectedTracks(window.token === "kept" ? 0 : 999);'))
    receive_until(lambda: (1, 'CountSelectedTracks', [0]) in results)
    assert set(first_children).issubset(children(parents[1])), 'WebKit was not reparented'
    send(dict(id=2, op='open', uri=page.as_uri(), script=script, lifecycleReload=True))
    geometry(2, parents[0])
    receive_until(lambda: (2, 'CountTracks', [0]) in results)
    if idle_target:
        assert (2, 'container', True) in results
        filling = True
        receive_until(lambda: idle_state is not None and idle_state['taken'] == 2 and idle_state['ready'] == idle_target)
        filling = False
        send(dict(id=0, op='idle-pool', target=0))
        receive_until(lambda: idle_state['target'] == 0 and idle_state['ready'] == 0 and idle_state['pending'] == 0)
    send(dict(id=1, op='close'))
    send(dict(id=2, op='eval', script='reaper.GetTrack(0, 0);'))
    receive_until(lambda: (2, 'GetTrack', [0, 0]) in results)
    hellos = sum(row[1] == '__reawebHello' for row in results)
    send(dict(id=2, op='eval', script='location.hash="fragment"; reaper.CountSelectedTracks(43);'))
    receive_until(lambda: (2, 'CountSelectedTracks', [43]) in results)
    assert not any(row[1] == 'reload-request' for row in results), 'Fragment triggered cleanup'
    for attempt in (1, 2):
        send(dict(id=2, op='eval', script='location.reload();'))
        receive_until(lambda: sum(row[1] == 'reload-request' for row in results) == attempt)
        assert sum(row[1] == '__reawebHello' for row in results) == hellos, 'Reload bypassed host cleanup'
        send(dict(id=2, op='reload'))
        receive_until(lambda: sum(row[1] == '__reawebHello' for row in results) > hellos)
        hellos += 1
    send(dict(id=2, op='close'))
    print('WebKitGTK: resize/background/theme/transparency, JS round-trip, host focus/blur, two windows, docking after old parent destruction, preserved page state, two host-gated reloads and independent close passed')
finally:
    parent.close()
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        if os.waitpid(process, os.WNOHANG)[0]: break
        time.sleep(.02)
    else:
        os.kill(process, signal.SIGKILL)
        os.waitpid(process, 0)
    for window in parents: x.XDestroyWindow(display, window)
    x.XCloseDisplay(display)
