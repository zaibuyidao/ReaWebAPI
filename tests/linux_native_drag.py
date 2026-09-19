"""Real GTK/WebKit native drag round trips in disposable test windows; no REAPER project."""
import ctypes as C
import json
import os
from pathlib import Path
import select
import signal
import socket
import time
import uuid

os.environ['GDK_BACKEND'] = 'x11'
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, Gdk, GLib

root = Path(__file__).resolve().parents[1]
build = root / os.environ.get('REAWEB_TEST_BUILD_DIR', 'build-linux')
fixture = build / ('native-drag-' + uuid.uuid4().hex)
fixture.mkdir()
page = fixture / 'index.html'
page.write_text('<!doctype html><style>body{user-select:none}</style><h1>Native drag test</h1>')
audio = fixture / 'unicode 空 格.wav'
audio.write_bytes(b'test fixture')  # Format is irrelevant to native path transfer.
host, target = Gtk.Window(title='ReaWebAPI test source'), Gtk.Window(title='ReaWebAPI test target')
host.set_default_size(360, 260); host.move(30, 30)
target.set_default_size(360, 260); target.move(450, 30)
embed = Gtk.Socket(); host.add(embed)
box = Gtk.EventBox(); box.add(Gtk.Label(label='Native file/text target and source')); target.add(box)
host.show_all(); target.show_all()
targets = [Gtk.TargetEntry.new('text/uri-list', 0, 1), Gtk.TargetEntry.new('UTF8_STRING', 0, 2)]
box.drag_dest_set(Gtk.DestDefaults.ALL, targets, Gdk.DragAction.COPY)
box.drag_source_set(Gdk.ModifierType.BUTTON1_MASK, targets, Gdk.DragAction.COPY)
received, source_payload = [], {'files': [], 'text': 'incoming text 你好'}
def got_data(widget, context, x, y, selection, info, timestamp):
    received.append({'files': selection.get_uris() or [], 'text': selection.get_text() or ''})
def give_data(widget, context, selection, info, timestamp):
    if source_payload['files']: selection.set_uris(source_payload['files'])
    else: selection.set_text(source_payload['text'], -1)
box.connect('drag-data-received', got_data)
box.connect('drag-data-get', give_data)

xlib = C.CDLL('libX11.so.6'); xtest = C.CDLL('libXtst.so.6')
xlib.XOpenDisplay.argtypes = [C.c_char_p]; xlib.XOpenDisplay.restype = C.c_void_p
xlib.XFlush.argtypes = [C.c_void_p]; xlib.XCloseDisplay.argtypes = [C.c_void_p]
xtest.XTestFakeMotionEvent.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_ulong]
xtest.XTestFakeButtonEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
display = xlib.XOpenDisplay(None); assert display, 'X11 is required'
pointer = Gdk.Display.get_default().get_default_seat().get_pointer()
_, old_x, old_y = pointer.get_position()
parent, child = socket.socketpair()
helper = str(build / 'reawebapi-webview-x86_64')
process = os.posix_spawn(helper, [helper, str(fixture / 'profile')], dict(os.environ),
    file_actions=[(os.POSIX_SPAWN_DUP2, child.fileno(), 3), (os.POSIX_SPAWN_CLOSE, child.fileno())])
child.close(); parent.setblocking(False)
buffer, messages = b'', []
def send(value): parent.sendall((json.dumps(value) + '\n').encode())
def pump(duration=.1):
    global buffer
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        while Gtk.events_pending(): Gtk.main_iteration_do(False)
        if select.select([parent], [], [], .005)[0]:
            data = parent.recv(65536); assert data, 'WebKit helper exited'
            buffer += data
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                message = json.loads(line); messages.append(message)
                assert message['op'] != 'error', message
def until(predicate, seconds=8):
    deadline = time.monotonic() + seconds
    while not predicate() and time.monotonic() < deadline: pump(.02)
    assert predicate(), (messages, received)
def move(x, y):
    xtest.XTestFakeMotionEvent(display, -1, int(x), int(y), 0); xlib.XFlush(display); pump(.06)
def button(down):
    xtest.XTestFakeButtonEvent(display, 1, int(down), 0); xlib.XFlush(display); pump(.12)
def point(widget):
    origin = widget.get_window().get_origin()
    return origin[-2] + 90, origin[-1] + 100
def travel(start, end):
    for n in range(1, 9): move(start[0] + (end[0]-start[0])*n/8, start[1] + (end[1]-start[1])*n/8)
def result(token):
    return next((m['response'] for m in messages if m['op'] == 'desktop-result' and m['request'] == token), None)

try:
    until(lambda: any(m['op'] == 'ready' for m in messages))
    send(dict(id=1, op='open', uri=page.as_uri(), script=''))
    send(dict(id=1, op='geometry', parent=embed.get_id(), x=0, y=0, width=350, height=250, visible=True))
    pump(1)
    # No mouse button: the native backend must reject instead of starting an unowned drag.
    send(dict(id=1, op='desktop', request='no-gesture', method='ReaWeb_Drag', args={'files': [], 'text': 'test'}))
    until(lambda: result('no-gesture') is not None)
    assert result('no-gesture')['error']['code'] == 'DRAG_GESTURE_REQUIRED'
    for token, payload in [('files', {'files': [str(audio)], 'text': ''}), ('text', {'files': [], 'text': 'outgoing 你好'})]:
        start, end = point(embed), point(box)
        move(*start); button(True)
        send(dict(id=1, op='desktop', request=token, method='ReaWeb_Drag', args=payload)); pump(.15)
        travel(start, end); button(False)
        until(lambda: result(token) is not None)
        assert result(token) == {'result': True}, result(token)
        assert received[-1]['files'] == [audio.as_uri()] if token == 'files' else received[-1]['text'] == payload['text'], received
    # Standard native GTK source -> WebKit's native receive handler.
    send(dict(id=1, op='drop-enabled', enabled=True)); pump()
    for mode in ('files', 'text'):
        source_payload = {'files': [audio.as_uri()] if mode == 'files' else [], 'text': 'incoming 你好'}
        box.drag_source_set(Gdk.ModifierType.BUTTON1_MASK, [targets[0 if mode == 'files' else 1]], Gdk.DragAction.COPY)
        before = len([m for m in messages if m['op'] == 'drop'])
        start, end = point(box), point(embed)
        move(*start); button(True); travel(start, end); button(False)
        until(lambda: len([m for m in messages if m['op'] == 'drop']) > before)
        payload = [m for m in messages if m['op'] == 'drop'][-1]['payload']
        assert payload['files'] == [str(audio)] if mode == 'files' else payload['text'] == source_payload['text'], payload
        assert 0 <= payload['x'] <= 350 and 0 <= payload['y'] <= 250, payload
    print('GTK native drag: outgoing/incoming Unicode files and text, copy results, coordinates and gesture rejection passed')
finally:
    xtest.XTestFakeButtonEvent(display, 1, 0, 0)
    xtest.XTestFakeMotionEvent(display, -1, old_x, old_y, 0); xlib.XFlush(display)
    parent.close()
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        if os.waitpid(process, os.WNOHANG)[0]: break
        time.sleep(.02)
    else: os.kill(process, signal.SIGKILL); os.waitpid(process, 0)
    host.destroy(); target.destroy(); xlib.XCloseDisplay(display)
