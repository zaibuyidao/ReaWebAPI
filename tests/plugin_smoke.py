"""Native plugin ABI test; Windows --webview also exercises WebView2 with a mock REAPER host."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import sys
import shutil
import socket
import subprocess
import urllib.request
import threading
import time
import uuid

parser = argparse.ArgumentParser()
parser.add_argument('--dll', type=Path, required=True)
parser.add_argument('--webview', action='store_true')
parser.add_argument('--latency', action='store_true', help='Measure startup and API round-trips with real WebView2 and a 30 ms mock host tick')
parser.add_argument('--demo', action='store_true', help='Exercise the shipped demo UI in the real WebView')
parser.add_argument('--starter', action='store_true', help='Exercise the SDK starter in the real WebView')
parser.add_argument('--dev', action='store_true', help='Exercise Vite modules, Worker, fetch and CSS HMR through OpenDev')
parser.add_argument('--modern', action='store_true', help='Exercise the built TypeScript template')
parser.add_argument('--runtime', action='store_true', help='Exercise Web Runtime v1 and per-App browser storage')
parser.add_argument('--studio', action='store_true', help='Exercise the v0.1.8 Runtime Studio UI')
parser.add_argument('--resource-root', type=Path, help='Reuse a dedicated test resource directory across host processes')
parser.add_argument('--empty', action='store_true', help='Use an empty project for demo tests')
args = parser.parse_args()
if args.dev: args.modern = True
server_process = None
window_count = 1 if args.demo or args.starter or args.modern or args.studio else 2
if args.webview and sys.platform != 'win32':
    raise SystemExit('--webview requires Windows; the ABI test supports all platforms')

root = args.resource_root.resolve() if args.resource_root else args.dll.resolve().parent / ('plugin-smoke-resource-' + str(os.getpid()))
root.mkdir(parents=True, exist_ok=True)
resource = C.create_string_buffer(str(root).encode('utf-8'))
version = C.create_string_buffer(b'7.mock')
guid = C.create_string_buffer(bytes(range(16)), 16)
project, track = 0x12345000, 0x12346000
item, take, envelope, accessor = 0x12347000, 0x12348000, 0x12349000, 0x1234a000
registrations, functions, callbacks, messages = {}, {}, [], []
name_calls = 0
main_thread = threading.get_ident()
docked, dock_events, dock_failures = set(), [], []
owner_window, docker_window = None, None
if args.webview:
    user32 = C.windll.user32
    user32.CreateWindowExW.argtypes = [W.DWORD, W.LPCWSTR, W.LPCWSTR, W.DWORD, C.c_int, C.c_int, C.c_int, C.c_int, W.HWND, W.HMENU, W.HINSTANCE, C.c_void_p]
    user32.CreateWindowExW.restype = W.HWND
    user32.GetWindowLongPtrW.argtypes = [W.HWND, C.c_int]
    user32.GetWindowLongPtrW.restype = C.c_ssize_t
    user32.SetWindowLongPtrW.argtypes = [W.HWND, C.c_int, C.c_ssize_t]
    user32.SetParent.argtypes = [W.HWND, W.HWND]
    user32.SetWindowPos.argtypes = [W.HWND, W.HWND, C.c_int, C.c_int, C.c_int, C.c_int, W.UINT]
    user32.SetForegroundWindow.argtypes = [W.HWND]
    user32.GetWindow.argtypes = [W.HWND, W.UINT]
    user32.GetWindow.restype = W.HWND
    user32.GetClassNameW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
    user32.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
    user32.DestroyWindow.argtypes = [W.HWND]
    owner_window = user32.CreateWindowExW(0, 'STATIC', 'Mock REAPER - owner test', 0x10CF0000, 40, 40, 960, 720, None, None, None, None)
    docker_window = user32.CreateWindowExW(0, 'STATIC', 'Mock Docker', 0x50000000, 0, 0, 640, 600, owner_window, None, None, None)
    assert owner_window and docker_window

def api(name, result, *params):
    def wrap(fn):
        callback = C.CFUNCTYPE(result, *params)(fn)
        callbacks.append(callback)
        functions[name.encode()] = C.cast(callback, C.c_void_p).value
        return fn
    return wrap

@api('GetResourcePath', C.c_void_p)
def resource_path():
    return C.addressof(resource)

@api('GetAppVersion', C.c_void_p)
def app_version():
    return C.addressof(version)

@api('ShowConsoleMsg', None, C.c_char_p)
def console(message):
    text = message.decode('utf-8', errors='replace')
    messages.append(text)
    print(text, end='')

@api('EnumProjects', C.c_void_p, C.c_int, C.c_void_p, C.c_int)
def enum_projects(index, buffer, size):
    return project

@api('CountTracks', C.c_int, C.c_void_p)
@api('CountSelectedTracks', C.c_int, C.c_void_p)
def count_tracks(proj):
    return 0 if args.empty else 1

@api('GetTrack', C.c_void_p, C.c_void_p, C.c_int)
@api('GetSelectedTrack', C.c_void_p, C.c_void_p, C.c_int)
def get_track(proj, index):
    return track if index == 0 and not args.empty else None

@api('ValidatePtr2', C.c_bool, C.c_void_p, C.c_void_p, C.c_char_p)
def valid(proj, pointer, kind):
    return (pointer == project and kind == b'ReaProject*') or (proj in (None, project) and {b'MediaTrack*': track,b'MediaItem*':item,b'MediaItem_Take*':take,b'TrackEnvelope*':envelope}.get(kind)==pointer)


@api('ValidatePtr', C.c_bool, C.c_void_p, C.c_char_p)
def valid_global(pointer, kind):
    return valid(project,pointer,kind)

@api('genGuid', None, C.c_void_p)
def gen_guid(p): C.memmove(p,guid,16)

@api('stringToGuid', None, C.c_char_p, C.c_void_p)
def string_guid(text,p): C.memmove(p,uuid.UUID(text.decode()).bytes_le,16)

@api('EnsureNotCompletelyOffscreen', None, C.c_void_p)
def rect(r): pass

@api('GetTrackEnvelope', C.c_void_p, C.c_void_p, C.c_int)
def get_env(p,i): return envelope if i==0 else None

@api('GetTrackMediaItem', C.c_void_p, C.c_void_p, C.c_int)
def get_item(p,i): return item if i==0 else None

@api('GetActiveTake', C.c_void_p, C.c_void_p)
def active_take(p): return take

@api('TakeIsMIDI', C.c_bool, C.c_void_p)
def is_midi(p): return True

buffers=[]
@api('realloc_cmd_register_buf', C.c_int, C.POINTER(C.c_void_p), C.POINTER(C.c_int))
def register_buffer(pointer,size):
    buffers.append([pointer,size,None])
    return len(buffers)-1

@api('realloc_cmd_clear', None, C.c_int)
def clear_buffer(token):
    del buffers[token:]

midi_bytes=bytes(range(256))*300
@api('MIDI_GetAllEvts', C.c_bool, C.c_void_p, C.c_void_p, C.POINTER(C.c_int))
def all_events(t,buffer,size):
    if size[0]<len(midi_bytes):
        for record in buffers:
            if record[0][0]==buffer:
                record[2]=C.create_string_buffer(midi_bytes)
                record[0][0]=C.addressof(record[2])
                record[1][0]=len(midi_bytes)
                extra_calls.add('binary-resize')
                return True
        return False
    C.memmove(buffer,midi_bytes,len(midi_bytes));size[0]=len(midi_bytes);return True

@api('CreateTrackAudioAccessor', C.c_void_p, C.c_void_p)
def create_accessor(t): return accessor

@api('GetAudioAccessorStartTime', C.c_double, C.c_void_p)
def accessor_start(p): return 0.0

@api('GetAudioAccessorSamples', C.c_int, C.c_void_p, C.c_int, C.c_int, C.c_double, C.c_int, C.POINTER(C.c_double))
def samples(p,sr,nch,start,count,buffer):
    for i in range(nch*count): buffer[i]=(-1 if i%2 else 1)*0.25
    extra_calls.add('array-write')
    return 1

@api('DestroyAudioAccessor', None, C.c_void_p)
def destroy_accessor(p): extra_calls.add('accessor-release')

@api('GetTrackGUID', C.c_void_p, C.c_void_p)
def get_guid(pointer):
    return C.addressof(guid)

@api('GetTrackName', C.c_bool, C.c_void_p, C.c_void_p, C.c_int)
def get_name(pointer, buffer, size):
    global name_calls
    if threading.get_ident() != main_thread:
        return False
    name_calls += 1
    name = 'Guitar 吉他 "A"'.encode('utf-8') + b'\0'
    C.memmove(buffer, name, min(size, len(name)))
    return True

pan_value, project_changes = 0.0, 0

@api('GetMediaTrackInfo_Value', C.c_double, C.c_void_p, C.c_char_p)
def get_value(pointer, key):
    if key == b'I_CUSTOMCOLOR': return color_value
    if key == b'I_NCHAN': return 2.0
    if key == b'D_PAN': return pan_value
    return 1.0

@api('Track_GetPeakInfo', C.c_double, C.c_void_p, C.c_int)
def track_peak(pointer, channel):
    assert threading.get_ident() == main_thread
    return 0.5 if channel == 0 else 0.0

@api('SetMediaTrackInfo_Value', C.c_bool, C.c_void_p, C.c_char_p, C.c_double)
def set_value(pointer, key, value):
    global color_value, pan_value, project_changes
    if key == b'I_CUSTOMCOLOR': color_value = int(value)
    if key == b'D_PAN' and value != pan_value:
        pan_value = value
        project_changes += 1
    return True

@api('GetProjectStateChangeCount', C.c_int, C.c_void_p)
def change_count(proj):
    return project_changes

@api('Undo_BeginBlock2', None, C.c_void_p)
def begin_undo(proj):
    pass

@api('Undo_EndBlock2', None, C.c_void_p, C.c_char_p, C.c_int)
def end_undo(proj, label, flags):
    pass

@api('PreventUIRefresh', None, C.c_int)
def prevent_refresh(value):
    pass

project_title = C.create_string_buffer('API 工程.rpp'.encode())
marker_title = C.create_string_buffer('段落 A'.encode())
cursor_value, color_value = 1.25, 0
extra_calls = set()

@api('GetProjectName', None, C.c_void_p, C.c_void_p, C.c_int)
def project_name(proj, buffer, size):
    extra_calls.add('project')
    C.memmove(buffer, project_title, min(size, len(project_title)))

@api('GetCursorPosition', C.c_double)
def cursor_position():
    return cursor_value

@api('GetPlayPosition', C.c_double)
def play_position():
    return 2.5

@api('GetPlayState', C.c_int)
def play_state():
    return 1

@api('Master_GetTempo', C.c_double)
def tempo():
    return 120

@api('SetEditCurPos', None, C.c_double, C.c_bool, C.c_bool)
def set_cursor(time, move, seek):
    global cursor_value
    assert threading.get_ident() == main_thread and not seek
    cursor_value = time
    extra_calls.add('cursor')

@api('TimeMap2_timeToBeats', C.c_double, C.c_void_p, C.c_double, C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_double), C.POINTER(C.c_int))
def beats(proj, time, measures, cml, fullbeats, denominator):
    measures[0], cml[0], fullbeats[0], denominator[0] = 0, 4, time * 2, 4
    extra_calls.add('beats')
    return time * 2

@api('CountProjectMarkers', C.c_int, C.c_void_p, C.POINTER(C.c_int), C.POINTER(C.c_int))
def count_markers(proj, markers, regions):
    if args.empty:
        markers[0], regions[0] = 0, 0
        return 0
    markers[0], regions[0] = 1, 1
    return 2

@api('EnumProjectMarkers3', C.c_int, C.c_void_p, C.c_int, C.POINTER(C.c_bool), C.POINTER(C.c_double), C.POINTER(C.c_double), C.POINTER(C.c_void_p), C.POINTER(C.c_int), C.POINTER(C.c_int))
def enum_markers(proj, index, region, start, end, name, number, color):
    extra_calls.add('markers')
    if args.empty or index >= 2: return 0
    region[0], start[0], end[0], name[0], number[0], color[0] = index == 1, 1.25, 2.5, C.addressof(marker_title), 10 + index, 0
    return 1

@api('GetTrackColor', C.c_int, C.c_void_p)
def get_color(track):
    return color_value

@api('SetTrackColor', None, C.c_void_p, C.c_int)
def set_color(track, color):
    global color_value
    assert threading.get_ident() == main_thread
    color_value = (color & 0xffffff) | 0x1000000
    extra_calls.add('color')

@api('ColorToNative', C.c_int, C.c_int, C.c_int, C.c_int)
def color_to_native(r, g, b):
    return r | (g << 8) | (b << 16)

@api('ColorFromNative', None, C.c_int, C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_int))
def color_from_native(value, r, g, b):
    r[0], g[0], b[0] = value & 255, (value >> 8) & 255, (value >> 16) & 255

@api('TrackFX_GetCount', C.c_int, C.c_void_p)
def fx_count(track):
    return 1

@api('TrackFX_GetFXName', C.c_bool, C.c_void_p, C.c_int, C.c_void_p, C.c_int)
def fx_name(track, index, buffer, size):
    C.memmove(buffer, b'EQ\0', min(size, 3))
    return index == 0

@api('TrackFX_GetNumParams', C.c_int, C.c_void_p, C.c_int)
def fx_num_params(track, index):
    return 1 if index == 0 else 0

@api('TrackFX_GetParam', C.c_double, C.c_void_p, C.c_int, C.c_int, C.POINTER(C.c_double), C.POINTER(C.c_double))
def fx_param(track, fx, param, minimum, maximum):
    minimum[0], maximum[0] = 0, 1
    extra_calls.add('fx')
    return .5

@api('Dock_UpdateDockID', None, C.c_char_p, C.c_int)
def remember_dock(identifier, index):
    pass

@api('UpdateArrange', None)
def update():
    pass

@api('DockWindowAddEx', None, C.c_void_p, C.c_char_p, C.c_char_p, C.c_bool)
def dock_add(window, title, identifier, show):
    dock_events.append('dock')
    docked.add(window)
    if args.webview:
        if user32.GetWindowLongPtrW(window, -8) != owner_window:
            dock_failures.append('Floating owner missing before docking')
        user32.SetParent(window, docker_window)
        user32.SetWindowLongPtrW(window, -16, 0x52000000)
        user32.SetWindowPos(window, None, 0, 0, 620, 580, 0x24)
        user32.SendMessageW(window, 0x80, 0, 0)
        user32.SendMessageW(window, 0x80, 1, 0)

@api('DockWindowRemove', None, C.c_void_p)
def dock_remove(window):
    dock_events.append('undock')
    docked.discard(window)
    if args.webview:
        user32.SetParent(window, None)
        user32.SendMessageW(window, 0x80, 0, 0)
        user32.SendMessageW(window, 0x80, 1, 0)

@api('DockIsChildOfDock', C.c_int, C.c_void_p, C.POINTER(C.c_bool))
def dock_index(window, floating):
    floating[0] = False
    return 0 if window in docked else -1

@api('DockWindowActivate', None, C.c_void_p)
def dock_activate(window):
    pass

REGISTER = C.CFUNCTYPE(C.c_int, C.c_char_p, C.c_void_p)
GETFUNC = C.CFUNCTYPE(C.c_void_p, C.c_char_p)

@REGISTER
def register(name, pointer):
    if name.startswith(b'-'):
        registrations.pop(name[1:], None)
    else:
        registrations[name] = pointer
    return 1

@GETFUNC
def get_func(name):
    return functions.get(name)

class Info(C.Structure):
    _fields_ = [('version', C.c_int), ('hwnd', C.c_void_p), ('register', REGISTER), ('get_func', GETFUNC)]

library = C.CDLL(str(args.dll.resolve()))
entry = library.ReaperPluginEntry
entry.argtypes = [C.c_void_p, C.POINTER(Info)]
entry.restype = C.c_int
info = Info(0x20E, owner_window, register, get_func)
assert entry(None, C.byref(info)) == 1
try:
    assert len(registrations) == 43, list(registrations)
    assert b'csurf_inst' in registrations
    open_window = C.CFUNCTYPE(C.c_int, C.c_char_p)(registrations[b'API_ReaWeb_Open'])
    for prefix in (b'API_', b'APIvararg_', b'APIdef_'):
        assert prefix + b'ReaWeb_Open' in registrations
        assert prefix + b'ReaWebOpen' not in registrations
    is_open = C.CFUNCTYPE(C.c_bool, C.c_int)(registrations[b'API_ReaWeb_IsOpen'])
    close_window = C.CFUNCTYPE(C.c_bool, C.c_int)(registrations[b'API_ReaWeb_Close'])
    get_error = C.CFUNCTYPE(C.c_char_p)(registrations[b'API_ReaWeb_GetLastError'])
    timer = C.CFUNCTYPE(None)(registrations[b'timer'])
    dev_open = C.CFUNCTYPE(C.c_int, C.c_char_p)(registrations[b'API_ReaWeb_OpenDev'])
    assert dev_open(b'https://example.com/') == 0

    class ProjectHooks(C.Structure):
        _fields_ = [('process', C.c_void_p), ('save', C.c_void_p), ('begin', C.c_void_p), ('data', C.c_void_p)]
    hooks = C.cast(registrations[b'projectconfig'], C.POINTER(ProjectHooks)).contents
    begin_project = C.CFUNCTYPE(None, C.c_bool, C.c_void_p)(hooks.begin)
    begin_project(True, registrations[b'projectconfig'])
    begin_project(False, registrations[b'projectconfig'])
    window_info = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_ssize_t)(registrations[b'hwnd_info'])
    assert window_info(owner_window, 0) == 0 and window_info(owner_window, 2) == 0
    captured_seen = False
    assert open_window(b'missing.html') == 0
    saved_error = get_error()
    timer()
    assert b'does not exist' in saved_error and saved_error == get_error()
    missing = C.create_string_buffer(b'missing.html')
    arguments = (C.c_void_p * 1)(C.addressof(missing))
    vararg = C.CFUNCTYPE(C.c_void_p, C.POINTER(C.c_void_p), C.c_int)(registrations[b'APIvararg_ReaWeb_Open'])
    assert not vararg(arguments, 1)
    assert not is_open(999)
    assert saved_error == get_error()
    host_send = C.CFUNCTYPE(C.c_bool, C.c_int, C.c_char_p)(registrations[b'API_ReaWeb_Send'])
    host_receive = C.CFUNCTYPE(C.c_char_p, C.c_int)(registrations[b'API_ReaWeb_Receive'])
    assert not host_send(999, b'closed') and b'closed or unknown' in get_error()
    assert host_receive(999) == b'' and b'closed or unknown' in get_error()
    send_args = (C.c_void_p * 2)(999, C.addressof(missing))
    send_vararg = C.CFUNCTYPE(C.c_void_p, C.POINTER(C.c_void_p), C.c_int)(registrations[b'APIvararg_ReaWeb_Send'])
    receive_vararg = C.CFUNCTYPE(C.c_char_p, C.POINTER(C.c_void_p), C.c_int)(registrations[b'APIvararg_ReaWeb_Receive'])
    assert not send_vararg(send_args, 2) and receive_vararg(send_args, 1) == b''
    if args.webview and args.runtime:
        from web_runtime_browser import run_windows
        run_windows(globals())
        args.webview = False
    if args.webview and args.latency:
        from web_latency import run_windows
        run_windows(globals())
        args.webview = False
    if args.webview:
        folder = root / 'Scripts' / ('development' if args.dev else '空 格#%')
        folder.mkdir(parents=True, exist_ok=True)
        page = folder / 'index.html'
        page.write_text('<!doctype html><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" '
                        'content="default-src &#39;none&#39;; script-src &#39;self&#39;"><script src="app.js" defer></script>'
                        '<h1>ReaWebAPI native smoke test</h1>', encoding='utf-8')
        expected_name = json.dumps('Guitar 吉他 "A"')
        (folder / 'app.js').write_text('''(async () => {
  const capabilities = await reaper.lifecycle.ready;
  const namespaces = ["window","theme","dialog","events","lifecycle","debug","fs","audio","clipboard","dragDrop","app","system","transaction"];
  if (Object.keys(reaper).length !== 743 || Object.keys(reaper).some(name => name.startsWith('ReaWeb')) || 'ready' in reaper)
    throw new Error('Runtime root exposes a flat API or has an incomplete Mirror');
  if (JSON.stringify(capabilities.runtime.namespaces) !== JSON.stringify(namespaces) || namespaces.some(name => !Object.isFrozen(reaper[name])))
    throw new Error('Incomplete Runtime namespaces');
  if (JSON.stringify(capabilities.runtime.reservedNamespaces) !== JSON.stringify([]))
    throw new Error('Every Runtime namespace is implemented');
  if ('ReaWebOpen' in reaper || capabilities.methods.includes('ReaWebOpen')) throw new Error('Removed entry name is still exposed');
  if (await reaper.app.getId() !== capabilities.webRuntime.appId || await reaper.app.getVersion() !== null ||
      !await reaper.app.getName() || !await reaper.app.getRootPath()) throw new Error('App metadata');
  if (await reaper.system.getPlatform() !== 'windows' || await reaper.system.getArchitecture() !== 'x64') throw new Error('System info');
  const dataPath = await reaper.app.getDataPath();
  await reaper.fs.writeText(dataPath + '/test.json', '{}', {overwrite:true});
  if (await reaper.fs.readText(dataPath + '/test.json') !== '{}') throw new Error('App writable data path');
  const dropped = () => { throw new Error('Unexpected native drop'); };
  const stopDrop = await reaper.events.on('native-drop', dropped);
  await reaper.events.off('native-drop', dropped); await stopDrop();
  try { await reaper.dragDrop.startText('without mouse gesture'); throw new Error('Drag should require gesture'); }
  catch (error) { if (error.code !== 'DRAG_GESTURE_REQUIRED') throw error; }
  const cleanupPath = 'cleanup-' + capabilities.windowId + '.txt';
  await reaper.lifecycle.on('before-reload', async () => {
    await reaper.fs.writeFile(cleanupPath, 'reload saved', {overwrite:true});
  });
  await reaper.lifecycle.on('before-close', async () => {
    await reaper.fs.writeFile(cleanupPath, 'close saved', {overwrite:true});
  });
  if (!sessionStorage.getItem('reloaded')) {
    location.hash = 'fragment';
    if (await reaper.CountTracks(0) !== 1) throw new Error('Fragment navigation broke bridge');
    sessionStorage.setItem('reloaded', 'yes');
    location.reload(); return;
  }
  const info = await reaper.debug.getDiagnostics();
  if (await reaper.fs.readFile(cleanupPath) !== 'reload saved') throw new Error('Reload cleanup did not finish');
  const theme = await reaper.theme.getColors();
  if (!theme.cssVariables['--reaper-background']) throw new Error('Missing theme variables');
  const size = await reaper.window.getSize();
  if (size.mode !== 'floating' || size.width <= 0) throw new Error('Window geometry');
  if (capabilities.windowId === 2) {
    const changed = await reaper.window.setSize(780, 620);
    if (changed.width !== 780 || changed.height !== 620) throw new Error('Native resize');
    await reaper.window.setPosition(80, 90);
    await reaper.window.hide(); await reaper.window.show();
  }
  if (info.stage !== 'ready' || info.backend !== 'WebView2' || info.documentGeneration < 2) throw new Error('Bad diagnostics');
  if (!(await reaper.window.getState()).keyboardCapture) throw new Error('Keyboard policy missing');
  const off = await reaper.events.on('windowstatechange', state => window.dockState = state.docked);
  const track = await reaper.GetSelectedTrack(0, 0);
  const meter = await reaper.audio.getTrackMeter(track);
  if (meter.channels !== 2 || meter.peak[0] !== 0.5 || meter.peakDb[1] !== null) throw new Error('Track meter');
  const [okName, name] = await reaper.GetTrackName(track);
  if (name !== EXPECTED || await reaper.CountTracks(0) !== 1) throw new Error('Wrong host data');
  if (await reaper.GetSelectedTrack(0, 99) !== null) throw new Error('Expected null track');
  if ((await reaper.system.getCapabilities()).api.implemented !== 730) throw new Error('Generated schema not connected');
  if (await reaper.GetProjectName(0) !== 'API 工程.rpp') throw new Error('Project string ABI');
  if (JSON.stringify(await reaper.CountProjectMarkers(0)) !== '[2,1,1]') throw new Error('Count outputs ABI');
  const marker = await reaper.EnumProjectMarkers3(0, 1);
  if (marker.length !== 7 || marker[1] !== true || marker[4] !== '段落 A' || marker[5] !== 11) throw new Error('Marker outputs ABI');
  if (JSON.stringify(await reaper.EnumProjectMarkers3(0, 2)) !== '[0,false,0,0,null,0,0]') throw new Error('End of enumeration');
  if (JSON.stringify(await reaper.TimeMap2_timeToBeats(0, 1.25)) !== '[2.5,0,4,2.5,4]') throw new Error('Beat outputs ABI');
  if (await reaper.SetEditCurPos(5, true, false) !== undefined || await reaper.GetCursorPosition() !== 5) throw new Error('Void result/cursor write');
  const nativeColor = await reaper.ColorToNative(37, 149, 211);
  if (JSON.stringify(await reaper.ColorFromNative(nativeColor)) !== '[37,149,211]') throw new Error('Color outputs ABI');
  await reaper.transaction.batch([{ method: 'SetTrackColor', args: [track, nativeColor | 0x1000000] }], { undoLabel: 'Color test' });
  if (await reaper.GetTrackColor(track) !== (nativeColor | 0x1000000)) throw new Error('Color readback');
  if (await reaper.TrackFX_GetCount(track) !== 1 || await reaper.TrackFX_GetNumParams(track, 0) !== 1) throw new Error('FX count ABI');
  if (JSON.stringify(await reaper.TrackFX_GetFXName(track, 0)) !== '[true,"EQ"]') throw new Error('FX name ABI');
  if (JSON.stringify(await reaper.TrackFX_GetParam(track, 0, 0)) !== '[0.5,0,1]') throw new Error('FX parameter ABI');
  const token = window.keepState = crypto.randomUUID();
  if (!await reaper.window.setDocked(true) || !await reaper.window.isDocked()) throw new Error('Dock failed');
  if (await reaper.window.setDocked(false) || await reaper.window.isDocked()) throw new Error('Undock failed');
  if (window.keepState !== token) throw new Error('Page state lost');
  if ((await reaper.GetTrackName(track))[1] !== name) throw new Error('Bridge broken after undocking');
  if (!await reaper.window.setDocked(true)) throw new Error('Second dock failed');
  document.querySelector('h1').textContent = 'PASS: ' + name;
  await off();
  await reaper.debug.log('Runtime log smoke');
  await reaper.window.close();
})().catch(async error => {
  document.querySelector('h1').textContent = error.stack;
  await reaper.debug.error(error);
});
'''.replace('EXPECTED', expected_name), encoding='utf-8')
        if args.studio:
            source = Path(__file__).resolve().parents[1] / 'runtime/runtime-demo'
            for name in ('index.html', 'app.js', 'style.css'):
                shutil.copyfile(source / name, folder / name)
            driver = r'''
(async () => {
  const until = async predicate => {
    const deadline = Date.now() + 15000;
    while (!await predicate()) {
      if (Date.now() > deadline) throw new Error('Timeout: ' + predicate.toString());
      await new Promise(resolve => setTimeout(resolve, 30));
    }
  };
  const el = id => document.getElementById(id);
  if (!el('status').textContent.startsWith('Connected to ReaWebAPI 0.1.8')) throw new Error(el('status').textContent);
  if (!sessionStorage.getItem('studioReload')) {
    sessionStorage.setItem('studioReload', 'yes');
    await reaper.window.reload(); return;
  }
  if (JSON.parse(localStorage.getItem('lastCleanup')).reason !== 'reload') throw new Error('Studio cleanup persistence');
  if (!document.documentElement.style.getPropertyValue('--reaper-background')) throw new Error('Studio theme');
  el('meter').click();
  await until(() => el('levels').textContent.includes('linear-amplitude'));
  if (JSON.parse(el('levels').textContent).peak[0] !== 0.5) throw new Error('Studio meter');
  el('resize').click();
  await until(async () => (await reaper.window.getSize()).width === 900);
  for (const state of [true, false, true]) {
    el('dock').click();
    await until(async () => (await reaper.window.getState()).docked === state);
  }
  if (document.documentElement.scrollWidth > innerWidth) throw new Error('Studio horizontal overflow');
  el('diagnostics').click();
  await until(() => el('status').textContent.includes('recentLogs'));
  await reaper.window.setTitle('STUDIO PASS');
  await reaper.window.close();
})().catch(async error => { await reaper.debug.error(error); });
'''
            with (folder / 'app.js').open('a', encoding='utf-8') as stream:
                stream.write(driver)
        if args.demo:
            source = Path(__file__).resolve().parents[1] / 'web'
            for name in ('index.html', 'app.js', 'style.css', 'logo.svg'):
                shutil.copyfile(source / name, folder / name)
            driver = r'''
(async () => {
  const until = async fn => {
    const deadline = Date.now() + 15000;
    while (!fn()) {
      if (Date.now() > deadline) throw new Error('Timed out: ' + fn.toString());
      await new Promise(r => setTimeout(r, 30));
    }
  };
  const el = id => document.getElementById(id);
  const click = id => { if (el(id).disabled) throw new Error('Disabled: ' + id); el(id).click(); };
  await until(() => el('project-name').textContent === 'API 工程.rpp' && (EMPTY_PROJECT || !el('apply-color').disabled));
  const iconStep = async step => {
    await reaper.window.setTitle('FAVICON:' + step);
    const deadline = Date.now() + 5000;
    while (!(await reaper.fs.stat('favicon-' + step + '.ok')).exists) {
      if (Date.now() > deadline) throw new Error('Native favicon timeout: ' + step);
      await new Promise(resolve => setTimeout(resolve, 20));
    }
  };
  if (sessionStorage.getItem('icon-reload')) {
    if ((await reaper.window.getState()).iconVisible !== false) throw new Error('Icon visibility lost after reload');
    await iconStep(10);
    await reaper.window.setIconVisible(true); await iconStep(11);
    await reaper.window.setDocked(true);
    await reaper.window.setTitle('DEMO PASS');
    await reaper.window.close(); return;
  }
  const cspViolations = [];
  const recordCsp = event => cspViolations.push(event.blockedURI);
  document.addEventListener('securitypolicyviolation', recordCsp);
  await reaper.debug.openDevTools();
  const settings = await fetch('/.well-known/appspecific/com.chrome.devtools.json');
  if (settings.status !== 200 || JSON.stringify(await settings.json()) !== '{}') throw new Error('DevTools settings request failed');
  // Give the native inspector time to issue its own automatic workspace probe.
  await new Promise(resolve => setTimeout(resolve, 1500));
  let inspector;
  const inspectorDeadline = Date.now() + 10000;
  do {
    inspector = (await reaper.debug.getDiagnostics()).devtools;
    if (inspector.visible && !inspector.pending) break;
    await new Promise(resolve => setTimeout(resolve, 30));
  } while (Date.now() < inspectorDeadline);
  if (!inspector.visible || inspector.pending || inspector.mode !== (inspector.embeddedSupported ? 'embedded' : 'floating'))
    throw new Error('DevTools layout did not open: ' + JSON.stringify(inspector));
  document.removeEventListener('securitypolicyviolation', recordCsp);
  if (cspViolations.length) throw new Error('DevTools CSP violation: ' + cspViolations.join(', '));
  click('clear-log');
  if (el('activity').textContent || !el('copy-log').disabled) throw new Error('Clear log failed');
  el('log-reaper').checked = true; el('log-reaper').dispatchEvent(new Event('change'));
  click('read-track');
  await until(() => !el('read-track').disabled);
  const readLog = el('activity').textContent;
  if (EMPTY_PROJECT ? !readLog.includes('[READ] No track selected.') :
      !readLog.includes('[READ] Track ' + JSON.stringify('Guitar 吉他 "A"') + ' · Pan 0.000 (center)')) throw new Error('Track snapshot log: ' + readLog);
  if (!el('read-track-result').textContent.startsWith('Logged:')) throw new Error('Missing button feedback');
  if (!(await reaper.debug.getLogs()).some(entry => entry.message.includes('[READ]'))) throw new Error('REAPER debug log mirror');
  el('log-reaper').checked = false; el('log-reaper').dispatchEvent(new Event('change'));
  if (!EMPTY_PROJECT) {
    el('pan').value = '0.5'; el('pan').dispatchEvent(new Event('input')); el('pan').dispatchEvent(new Event('change'));
    await until(() => el('activity').textContent.includes('→ +0.500 (50.0% R) · REAPER readback'));
    // A host-side change arrives through project events, without touching the slider.
    await reaper.SetMediaTrackInfo_Value(await reaper.GetSelectedTrack(0, 0), 'D_PAN', -0.25);
    await until(() => el('activity').textContent.includes('→ -0.250 (25.0% L) · REAPER readback'));
    click('center-pan');
    await until(() => el('activity').textContent.includes('→ 0.000 (center) · REAPER readback'));
  }
  if (el('activity').getBoundingClientRect().height > 210) throw new Error('Unbounded debug log');
  const logo = document.querySelector('.mark');
  await until(() => logo.complete && logo.naturalWidth > 0);
  if (!el('diagnostic-panel').hidden || el('diagnostics').getAttribute('aria-expanded') !== 'false') throw new Error('Diagnostics should start collapsed');
  click('diagnostics'); click('diagnostics');
  await new Promise(resolve => setTimeout(resolve, 200));
  if (!el('diagnostic-panel').hidden || el('diagnostics').getAttribute('aria-expanded') !== 'false') throw new Error('Late response reopened diagnostics');
  click('diagnostics');
  await until(() => el('diagnostic-backend').textContent === 'WebView2' && !el('refresh-diagnostics').disabled);
  if (el('diagnostic-panel').hidden || el('diagnostic-details').open || el('diagnostics').getAttribute('aria-expanded') !== 'true') throw new Error('Diagnostic disclosure state');
  const snapshot = JSON.parse(el('diagnostic-output').textContent);
  if (snapshot.api.implemented !== 730 || snapshot.api.bindings || snapshot.api.availableMethods) throw new Error('Diagnostic snapshot contains redundant API dump');
  el('diagnostic-details').open = true;
  if (el('diagnostic-output').getBoundingClientRect().height > 261) throw new Error('Diagnostic JSON is not bounded');
  click('refresh-diagnostics'); await until(() => !el('refresh-diagnostics').disabled);
  click('diagnostics');
  if (!el('diagnostic-panel').hidden || el('diagnostic-details').open) throw new Error('Diagnostics did not fold back');
  if (!el('api-coverage').textContent.includes('730 bound')) throw new Error('Coverage display');
  click('read-project'); await until(() => !el('read-project').disabled);
  if (EMPTY_PROJECT) {
    if (!el('read-fx').disabled || !el('apply-color').disabled || !el('marker-list').textContent.includes('No markers')) throw new Error('Empty project controls');
  } else {
    if (!el('marker-list').textContent.includes('段落 A')) throw new Error('Marker display');
    click('read-fx'); await until(() => !el('read-fx').disabled);
    if (!el('fx-list').textContent.includes('EQ') || !el('fx-list').textContent.includes('0.500')) throw new Error('FX display');
  }
  click('run-checks'); await until(() => !el('run-checks').disabled);
  if (el('check-results').querySelectorAll('.pass').length !== (EMPTY_PROJECT ? 7 : 10) || el('check-results').querySelector('.fail')) throw new Error(el('check-results').textContent);
  if (EMPTY_PROJECT && el('check-results').querySelectorAll('.skip').length !== 3) throw new Error('Empty project should skip track checks');
  if (!EMPTY_PROJECT) {
  el('track-color').value = '#2595d3'; click('apply-color'); await until(() => !el('apply-color').disabled);
  if (await reaper.GetTrackColor(await reaper.GetSelectedTrack(0, 0)) !== ((await reaper.ColorToNative(37,149,211)) | 0x1000000)) throw new Error('Apply color');
  click('reset-color'); await until(() => !el('reset-color').disabled);
  if (await reaper.GetTrackColor(await reaper.GetSelectedTrack(0, 0)) !== 0) throw new Error('Default color');
  }
  el('cursor-target').value = '7.25'; click('move-cursor'); await until(() => !el('move-cursor').disabled);
  if (el('cursor-position').textContent !== '7.250 s') throw new Error('Move cursor display');
  if (!el('error').hidden) throw new Error(el('error').textContent);
  if (document.documentElement.scrollWidth > innerWidth) throw new Error('Horizontal overflow');
  if (el('dock')) throw new Error('Demo should use the native docking menu');
  const favicon = document.querySelector('link[rel="icon"]');
  if (!favicon) throw new Error('Demo favicon declaration is missing');
  await iconStep(1);
  await reaper.fs.writeText('alternate.svg', '<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><rect width="16" height="16" fill="blue"/></svg>', {overwrite:true});
  favicon.href = 'alternate.svg'; await iconStep(2);
  favicon.remove(); await iconStep(3);
  favicon.href = 'logo.svg'; document.head.append(favicon); await iconStep(4);
  await reaper.window.setIcon('alternate.svg'); await iconStep(5);
  favicon.remove(); await new Promise(resolve => setTimeout(resolve, 200)); await iconStep(6);
  for (const step of [1, 2, 3]) {
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    await reaper.fs.writeText('viewport-' + step + '.json', JSON.stringify({width:innerWidth,height:innerHeight,scale:devicePixelRatio}), {overwrite:true});
    await reaper.window.setTitle('NATIVE DOCK:' + step);
    const deadline = Date.now() + 5000;
    while (await reaper.window.isDocked() !== (step !== 2)) {
      if (Date.now() > deadline) throw new Error('Native docking timeout');
      await new Promise(resolve => setTimeout(resolve, 20));
    }
    if (document.documentElement.scrollWidth > innerWidth) throw new Error('Docking horizontal overflow');
  }
  await reaper.window.setIconVisible(false);
  await reaper.window.setDocked(false); await iconStep(7);
  await reaper.window.setIcon('logo.svg'); await iconStep(8);
  await reaper.window.setIconVisible(true); await iconStep(9);
  await reaper.window.setIconVisible(false);
  sessionStorage.setItem('icon-reload', 'true');
  await reaper.window.reload();
})().catch(async error => { await reaper.window.setTitle('DEMO FAIL: ' + error.message.slice(0, 160)); });
'''
            with (folder / 'app.js').open('a', encoding='utf-8') as stream:
                stream.write(driver.replace('EMPTY_PROJECT', 'true' if args.empty else 'false'))
        if args.starter:
            source = Path(__file__).resolve().parents[1] / 'runtime/starter'
            for name in ('index.html', 'app.js', 'style.css'):
                shutil.copyfile(source / name, folder / name)
            driver = r'''
(async () => {
  const until = async predicate => {
    const deadline = Date.now() + 15000;
    while (!predicate()) {
      if (Date.now() > deadline) throw new Error('Timeout: ' + predicate.toString());
      await new Promise(resolve => setTimeout(resolve, 30));
    }
  };
  const el = id => document.getElementById(id);
  await until(() => !el('refresh').disabled && el('track-name').textContent === EXPECTED);
  if (!el('status').textContent.includes('REAPER APIs available')) throw new Error('Missing capability state');
  el('refresh').click();
  await until(() => el('track-name').textContent === EXPECTED);
  const state = window.starterState = 'preserved';
  if (el('dock')) throw new Error('Starter should use the native docking menu');
  for (const step of [1, 2, 3]) {
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    await reaper.fs.writeText('viewport-' + step + '.json', JSON.stringify({width:innerWidth,height:innerHeight,scale:devicePixelRatio}), {overwrite:true});
    await reaper.window.setTitle('NATIVE DOCK:' + step);
    const deadline = Date.now() + 5000;
    while (await reaper.window.isDocked() !== (step !== 2)) {
      if (Date.now() > deadline) throw new Error('Native docking timeout');
      await new Promise(resolve => setTimeout(resolve, 20));
    }
    if (window.starterState !== state) throw new Error('Docking lost document state');
  }
  if (document.documentElement.scrollWidth > innerWidth) throw new Error('Horizontal overflow');
  await reaper.window.setTitle('STARTER PASS');
  await reaper.window.close();
})().catch(async error => { await reaper.window.setTitle('STARTER FAIL: ' + error.message.slice(0, 160)); });
'''
            with (folder / 'app.js').open('a', encoding='utf-8') as stream:
                stream.write(driver.replace('EXPECTED', json.dumps('No track selected') if args.empty else expected_name))
        if args.modern:
            source = Path(__file__).resolve().parents[1] / 'runtime/modern/dist'
            for name in ('index.html', 'app.js', 'style.css', 'data.json'):
                shutil.copyfile(source / name, folder / name)
            driver = r'''
(async () => {
  const until = async predicate => {
    const deadline = Date.now() + 15000;
    while (!predicate()) {
      if (Date.now() > deadline) throw new Error('Timeout: ' + predicate.toString());
      await new Promise(resolve => setTimeout(resolve, 30));
    }
  };
  const el = id => document.getElementById(id);
  await until(() => el('status').textContent === 'Ready' && el('resources').textContent.includes('Worker sum: 10'));
  if (!el('project').textContent.includes('tracks')) throw new Error('Project data missing');
  el('gain').click(); await until(() => !el('gain').disabled);
  const bytes = Uint8Array.from({length:100000}, (_,i)=>i%256);
  await reaper.fs.writeFile('roundtrip.bin', bytes, {encoding:'binary'});
  const read = await reaper.fs.readFile('roundtrip.bin', {encoding:'binary'});
  if (read.length !== bytes.length || read.some((v,i)=>v!==bytes[i])) throw new Error('Binary file mismatch');
  await reaper.transaction.withUndo('Gesture smoke', async () => {
    const track = await reaper.GetSelectedTrack(0, 0);
    await reaper.SetMediaTrackInfo_Value(track, 'D_VOL', .5);
  });
  for (const state of [true, false, true]) {
    if (await reaper.window.setDocked(state) !== state) throw new Error('Dock failed');
  }
  if (document.documentElement.scrollWidth > innerWidth) throw new Error('Horizontal overflow');
  await reaper.window.setTitle('MODERN PASS');
  await reaper.window.close();
})().catch(async error => { await reaper.window.setTitle('MODERN FAIL: ' + error.message.slice(0,160)); });
'''
            if args.dev:
                source = Path(__file__).resolve().parents[1] / 'runtime/modern'
                shutil.copytree(source / 'src', folder / 'src', dirs_exist_ok=True)
                shutil.copytree(source / 'public', folder / 'public', dirs_exist_ok=True)
                html = (source / 'index.html').read_text(encoding='utf-8')
                page.write_text(html.replace('</body>', '<script type="module" src="/smoke-driver.ts"></script></body>'), encoding='utf-8')
                hmr = """
  const cssPath = STYLE_PATH;
  const css = await reaper.fs.readFile(cssPath);
  await reaper.fs.writeFile(cssPath, css + '\\n:root{--hmr-smoke:updated}', {overwrite:true});
  await until(() => getComputedStyle(document.documentElement).getPropertyValue('--hmr-smoke') === 'updated');
"""
                driver = driver.replace("  if (!el('project')", hmr.replace('STYLE_PATH', json.dumps(str(folder / 'src/style.css'))) + "  if (!el('project')")
                (folder / 'smoke-driver.ts').write_text(driver, encoding='utf-8')
                with socket.socket() as sock:
                    sock.bind(('127.0.0.1', 0))
                    port = sock.getsockname()[1]
                dev_url = f'http://127.0.0.1:{port}/'
                server_process = subprocess.Popen([shutil.which('node'), str(source / 'node_modules/vite/bin/vite.js'),
                    str(folder), '--host', '127.0.0.1', '--port', str(port), '--strictPort'],
                    stdout=(folder / 'vite.log').open('w', encoding='utf-8'), stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
                deadline = time.monotonic() + 15
                while True:
                    try:
                        with urllib.request.urlopen(dev_url, timeout=1): break
                    except OSError:
                        assert server_process.poll() is None and time.monotonic() < deadline, (folder / 'vite.log').read_text(encoding='utf-8', errors='replace')
                        time.sleep(.1)
            else:
                with (folder / 'app.js').open('a', encoding='utf-8') as stream:
                    stream.write(driver)
        state_folder = root / 'ReaWebAPI' / 'WindowState'
        state_folder.mkdir(parents=True, exist_ok=True)
        state_files = []
        for slot in range(window_count):
            entry_path = page.resolve().as_posix()
            if args.dev:
                url_hash = 14695981039346656037
                for byte in dev_url.encode(): url_hash = ((url_hash ^ byte) * 1099511628211) & ((1 << 64) - 1)
                entry_path = (root / 'Scripts' / f'.reaweb-dev-{url_hash:016x}-0.html').resolve().as_posix()
            hashed = 14695981039346656037
            for byte in entry_path.encode('utf-8'): hashed = ((hashed ^ byte) * 1099511628211) & ((1 << 64) - 1)
            file = state_folder / f'{hashed:016x}-{slot}.json'
            file.write_text(json.dumps(dict(schema=1, entry=entry_path, docked=False, dockId=0,
                placement=dict(x=900000, y=900000, width=860, height=640, maximized=slot == 0))), encoding='utf-8')
            state_files.append(file)
        messages.clear()
        ids = [dev_open(dev_url.encode()) if args.dev else open_window(str(page).encode('utf-8')) for _ in range(window_count)]
        assert all(ids), get_error()
        # Close a controller request before its asynchronous initialization completes.
        early = open_window(str(page).encode('utf-8'))
        assert early and close_window(early)
        user32 = C.windll.user32
        user32.PeekMessageW.argtypes = [C.POINTER(W.MSG), W.HWND, W.UINT, W.UINT, W.UINT]
        user32.TranslateMessage.argtypes = [C.POINTER(W.MSG)]
        user32.DispatchMessageW.argtypes = [C.POINTER(W.MSG)]
        native_steps = set()
        favicon_icons = {}
        for step in range(1, 12):
            (folder / f'favicon-{step}.ok').unlink(missing_ok=True)
        user32.GetWindowTextW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
        user32.GetDlgItem.argtypes = [W.HWND, C.c_int]
        user32.GetDlgItem.restype = W.HWND
        user32.GetClientRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
        user32.GetWindowRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
        user32.ScreenToClient.argtypes = [W.HWND, C.POINTER(W.POINT)]
        user32.FindWindowExW.argtypes = [W.HWND, W.HWND, W.LPCWSTR, W.LPCWSTR]
        user32.FindWindowExW.restype = W.HWND
        user32.IsWindowVisible.argtypes = [W.HWND]
        user32.SendMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
        user32.SendMessageW.restype = C.c_ssize_t
        deadline = time.monotonic() + 35
        while time.monotonic() < deadline:
            msg = W.MSG()
            for _ in range(100):
                if not user32.PeekMessageW(C.byref(msg), None, 0, 0, 1):
                    break
                user32.TranslateMessage(C.byref(msg))
                user32.DispatchMessageW(C.byref(msg))
            timer()
            user32.SetForegroundWindow(owner_window)
            # Owned floating windows must remain above their owner in the Z order.
            handles = []
            current = user32.GetWindow(owner_window, 0)  # GW_HWNDFIRST
            while current:
                handles.append(current)
                current = user32.GetWindow(current, 2)  # GW_HWNDNEXT
            for handle in handles:
                class_name = C.create_unicode_buffer(128)
                user32.GetClassNameW(handle, class_name, 128)
                process_id = W.DWORD()
                user32.GetWindowThreadProcessId(handle, C.byref(process_id))
                if process_id.value == os.getpid() and class_name.value == 'ReaWebAPI.Window' and handle not in docked:
                    captured_seen |= window_info(handle, 0) == 1 and window_info(handle, 1) == 1
                    assert user32.GetWindowLongPtrW(handle, -8) == owner_window, (handle, owner_window, user32.GetWindowLongPtrW(handle, -8))
                    assert handles.index(handle) < handles.index(owner_window), 'App fell behind REAPER after focus changed'
            if args.demo or args.starter:
                for handle in set(handles) | docked.copy():
                    title = C.create_unicode_buffer(256)
                    user32.GetWindowTextW(handle, title, 256)
                    if args.demo and title.value.startswith('FAVICON:') and int(title.value.split(':')[1]) not in favicon_icons:
                        step = int(title.value.split(':')[1])
                        small = user32.SendMessageW(handle, 0x7F, 0, 0)
                        large = user32.SendMessageW(handle, 0x7F, 1, 0)
                        previous = favicon_icons.get(step - 1)
                        hidden = step in (7, 8, 10)
                        correct = (not small and not large) if step == 3 or hidden else bool(small and large)
                        if hidden or step in (9, 11):
                            correct = correct and bool(user32.GetWindowLongPtrW(handle, -20) & 1) == hidden
                            assert user32.GetWindowLongPtrW(handle, -16) & 0x80000, 'System menu was removed'
                        if step in (2, 5):
                            correct = correct and small != previous
                        elif step == 6:
                            correct = correct and small == previous
                        if correct and step not in favicon_icons:
                            favicon_icons[step] = small
                            (folder / f'favicon-{step}.ok').write_text('ok', encoding='utf-8')
                    if not title.value.startswith('NATIVE DOCK:') or (handle, title.value) in native_steps:
                        continue
                    native_steps.add((handle, title.value))
                    assert not user32.GetDlgItem(handle, 0x1800), 'Removed docking toolbar is still present'
                    viewport = json.loads((folder / ('viewport-' + title.value.split(':')[1] + '.json')).read_text())
                    client = W.RECT()
                    assert user32.GetClientRect(handle, C.byref(client))
                    page_width = client.right
                    divider = user32.FindWindowExW(handle, None, 'ReaWebAPI.DevTools.Splitter', None)
                    if divider and user32.IsWindowVisible(divider):
                        split = W.RECT()
                        assert user32.GetWindowRect(divider, C.byref(split))
                        origin = W.POINT(split.left, split.top)
                        assert user32.ScreenToClient(handle, C.byref(origin))
                        page_width = origin.x
                        assert 0 < page_width < client.right
                    assert abs(viewport['width'] * viewport['scale'] - page_width) <= 1, (viewport, page_width)
                    assert abs(viewport['height'] * viewport['scale'] - client.bottom) <= 1, (viewport, client.bottom)
                    if args.demo:
                        assert user32.SendMessageW(handle, 0x7F, 0, 0), 'Demo small window icon is missing'
                        assert user32.SendMessageW(handle, 0x7F, 1, 0), 'Demo large window icon is missing'
                    user32.SendMessageW(handle, 0x112, 0x1800, 0)  # Title-bar system menu.
            if not any(is_open(window) for window in ids) and not docked:
                break
            time.sleep(0.01)
        assert captured_seen, 'Native keyboard capture hook was not active'
        diagnostics = C.CFUNCTYPE(C.c_char_p, C.c_int)(registrations[b'API_ReaWeb_GetDiagnostics'])
        assert (name_calls == 0 if args.empty or args.modern else name_calls > 0 if args.demo or args.starter or args.studio else name_calls == 4) and not any(is_open(window) for window in ids), (name_calls, messages, [diagnostics(id) for id in ids])
        if args.demo:
            assert json.loads(diagnostics(ids[0]))['window']['title'] == 'DEMO PASS'
            assert set(favicon_icons) == set(range(1, 12)), favicon_icons
            print('Window icons: favicon sync, explicit override, hidden replacement, docking restoration and reload persistence passed')
            assert any('[READ]' in message and '[ReaWebAPI]' in message for message in messages), 'Native console mirror missing'
            print('Demo debug log: explicit track snapshot, REAPER console mirror and clear passed' + ('' if args.empty else ', including confirmed Pan changes'))
            print('Shipped demo: managed DevTools layout opened, project settings returned 200 and no CSP violations')
            print('Shipped demo: empty project, 7 checks passed and 3 track checks skipped, cursor and docking passed' if args.empty else 'Shipped demo: 10 checks passed (including binary resize, GUID/RECT and audio array), project/marker/FX display, color write/read/reset, cursor write, native icons and title-bar docking passed')
        if args.studio:
            assert json.loads(diagnostics(ids[0]))['window']['title'] == 'STUDIO PASS'
            assert extra_calls == set(), extra_calls
            print('Runtime Studio: theme, selected track, peak meter, resize, docking, diagnostics and reload state persistence passed')
        elif args.modern:
            assert json.loads(diagnostics(ids[0]))['window']['title'] == 'MODERN PASS'
            assert extra_calls == {'project'}, extra_calls
            if args.dev: print('Development mode: loopback entry, Vite modules, HTTP fetch, Worker and live CSS HMR passed')
            print('Modern template: IIFE/CSS, local resources, Blob Worker, 100KB binary file round-trip, batch/managed Undo and docking passed')
        elif args.starter:
            assert json.loads(diagnostics(ids[0]))['window']['title'] == 'STARTER PASS'
            assert extra_calls == set(), extra_calls
            print('SDK starter: capability handshake, selected/empty track, refresh, docking, layout and close passed')
        else:
            assert extra_calls == ({'project', 'cursor', 'beats', 'markers'} if args.empty else ({'project', 'cursor', 'beats', 'markers', 'color', 'fx'} | ({'binary-resize','array-write','accessor-release'} if args.demo else set()))), extra_calls
        timer()
        assert not docked and not dock_failures and dock_events.count('dock') == window_count * (3 if args.demo else 2), (docked, dock_events, dock_failures)
        if not (args.demo or args.starter or args.modern or args.studio):
            assert sorted(messages) == [f'[ReaWebAPI] [App {id}] [info] Runtime log smoke\n' for id in ids], messages
            assert get_error() == saved_error, 'Runtime logs overwrote the last host error'
        elif args.demo:
            assert len(messages) == 2 and all(f'[ReaWebAPI] [App {ids[0]}] [info]' in text for text in messages), messages
            assert '[LOG]' in messages[0] and '[READ]' in messages[1], messages
            assert get_error() == saved_error, 'Demo logs overwrote the last host error'
        else:
            assert not messages, messages
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            states = [json.loads(file.read_text(encoding='utf-8')) for file in state_files]
            if all(state['placement']['x'] != 900000 and state['docked'] for state in states): break
            timer(); time.sleep(.01)
        assert all(state['placement']['x'] != 900000 and state['docked'] for state in states), states
        assert states[0]['placement']['maximized'] is (not args.studio), states
        assert list((root / 'ReaWebAPI' / 'Apps').glob('*/WebViewData'))
        if not (args.demo or args.starter or args.modern or args.studio):
            for id in ids:
                assert (folder / f'cleanup-{id}.txt').read_text() == 'close saved'
            print('Runtime SDK: native geometry, theme, meter and async reload/close persistence passed')
        print('WebView2: JS round-trip, Unicode, multi-window, shared UDF, dock/undock, owner restoration and close cleanup passed')
    print('Plugin ABI: exports, API registration, vararg wrapper, error retention and unload passed')
finally:
    if server_process:
        server_process.terminate()
        server_process.wait(timeout=10)
    entry(None, None)
    if owner_window:
        user32.DestroyWindow(owner_window)
    assert not registrations
