"""Exercise native overlay input and order in an isolated macOS REAPER profile."""
import argparse
import ctypes
import json
from pathlib import Path
import shutil
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--extension', type=Path, required=True)
p.add_argument('--probe', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--example', action='store_true')
a = p.parse_args()
session = subprocess.check_output(['ioreg', '-n', 'Root', '-d1'], text=True)
if '"CGSSessionScreenIsLocked"=Yes' in session:
    raise SystemExit('Unlock the macOS desktop before native overlay acceptance.')
root = a.output.resolve()
root.mkdir(parents=True, exist_ok=False)
(root / 'UserPlugins').mkdir()
(root / 'overlay-test.enabled').touch()
for source in (a.extension, a.probe):
    target = root / 'UserPlugins' / source.name
    shutil.copy2(source, target)
    subprocess.run(['codesign', '--force', '--sign', '-', str(target)], check=True, capture_output=True)
profile = Path.home() / 'Library/Application Support/REAPER'
for name in ('reaper-license.rk', 'reaper-install-rev.txt'):
    if (profile / name).exists():
        shutil.copy2(profile / name, root / name)
(root / 'EmptyVST').mkdir()
(root / 'reaper.ini').write_text('[reaper]\nmixwnd_vis=0\nverchk=0\nerrnowarn=5\nloadlastproj=0\nshowlastproj=0\nsplash=0\nhasrecentlyopened=1\nnewprojdo=0\nvst_scan=0\nvstfullstate=49989\nvstpath64=' + str(root / 'EmptyVST') + '\n[audioconfig]\nmode=4\n')
page = root / 'Scripts/Overlay'
page.mkdir(parents=True)
(page / 'app.json').write_text('{"id":"overlay-native-acceptance"}')
(page / 'index.html').write_text('''<!doctype html><style>html,body{background:transparent;margin:0}button{position:absolute;left:100px;top:100px;width:200px;height:160px;background:#f008}</style><button id="control">Drag</button><script>
window.inputEvents=[];for(const type of ['pointerdown','pointerup','pointermove','wheel'])addEventListener(type,e=>{inputEvents.push(type);if(type==='pointerdown')e.target.setPointerCapture(e.pointerId)});
</script>''')
if a.example:
    example=Path(__file__).resolve().parents[1]/'examples/windows/overlay'
    for source in example.iterdir():
        if source.suffix in ['.html','.js','.css']: shutil.copy2(source,page/source.name)
    (root/'reaper-kb.ini').write_text(''.join(f'KEY {flags} {key} 40042 0\n' for flags,key in [(1,32),(1,65),(5,65),(9,65),(13,65),(17,65),(1,37),(1,119)]))
    effect=root/'Effects/overlay';effect.mkdir(parents=True)
    (effect/'probe.jsfx').write_text('desc:Overlay native FX\nslider1:0.5<0,1,0.01>Gain\n@sample\nspl0*=slider1;spl1*=slider1;\n@gfx 300 200\ngfx_r=0.3;gfx_rect(0,0,300,200);\n')


def wait(fn, timeout=30):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if process.poll() is not None:
            raise RuntimeError('REAPER exited: ' + str(process.returncode))
        result = fn()
        if result:
            return result
        time.sleep(.1)
    raise AssertionError('Timed out')

def cmd(op, **args):
    (root / 'overlay-reply.json').unlink(missing_ok=True)
    (root / 'overlay-command.tmp').write_text(json.dumps(dict(op=op, **args)))
    (root / 'overlay-command.tmp').replace(root / 'overlay-command.json')
    wait(lambda: (root / 'overlay-reply.json').exists())
    result = json.loads((root / 'overlay-reply.json').read_text())
    assert not isinstance(result, dict) or 'error' not in result, result
    return result

def ev(script):
    return cmd('eval', script=script)

cg = ctypes.CDLL('/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics')
cf = ctypes.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
class Point(ctypes.Structure):
    _fields_ = [('x', ctypes.c_double), ('y', ctypes.c_double)]
cg.CGEventCreateMouseEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint32, Point, ctypes.c_uint32]
cg.CGEventCreateMouseEvent.restype = ctypes.c_void_p
cg.CGEventCreateScrollWheelEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int32]
cg.CGEventCreateScrollWheelEvent.restype = ctypes.c_void_p
cg.CGEventSetLocation.argtypes = [ctypes.c_void_p, Point]
cg.CGEventSetIntegerValueField.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int64]
cg.CGEventPost.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
cf.CFRelease.argtypes = [ctypes.c_void_p]
cg.CGEventCreate.argtypes = [ctypes.c_void_p]
cg.CGEventCreate.restype = ctypes.c_void_p
cg.CGEventGetLocation.argtypes = [ctypes.c_void_p]
cg.CGEventGetLocation.restype = Point

def cursor_position():
    event = cg.CGEventCreate(None)
    point = cg.CGEventGetLocation(event)
    cf.CFRelease(event)
    return point.x, point.y

last_mouse = [0, 0]
def mouse(kind, x, y, **args):
    position = cmd('point', x=x, y=y)
    point = Point(*position)
    # Pointer Lock keeps physical cursor position fixed while delivering deltas.
    if a.example and kind in ('drag', 'up') and ev('return !!document.pointerLockElement'):
        point = Point(*cursor_position())
    if kind == 'wheel':
        event = cg.CGEventCreateScrollWheelEvent(None, 1, 1, 3)
        cg.CGEventSetLocation(event, point)
    else:
        event = cg.CGEventCreateMouseEvent(None, dict(move=5,down=1,up=2,drag=6,rightDown=3,rightUp=4)[kind], point, 1 if kind.startswith('right') else 0)
        if kind in ('move', 'drag'):
            cg.CGEventSetIntegerValueField(event, 4, round(x-last_mouse[0]))
            cg.CGEventSetIntegerValueField(event, 5, round(y-last_mouse[1]))
    last_mouse[:] = [x, y]
    cg.CGEventPost(0, event); cf.CFRelease(event)
    time.sleep(.25)

def click(x, y):
    mouse('move', x, y)
    mouse('down', x, y)
    mouse('up', x, y)

def order(floating=False):
    time.sleep(.3)
    state = cmd('state')
    assert state['overlay'] < state['main'], state
    if floating:
        assert state['floating'] < state['overlay'], state
    return state

report = []
with (root / 'process.log').open('w') as log:
    process = subprocess.Popen(['/Applications/REAPER.app/Contents/MacOS/REAPER', '-newinst', '-cfgfile', str(root / 'reaper.ini'), '-new', '-ignoreerrors', '-nosplash', '-ApplePersistenceIgnoreState', 'YES'], stdout=log, stderr=log)
    try:
        wait(lambda: (root / 'overlay-ready').exists(), 120)
        if a.example:
            from overlay_acceptance import run_macos
            run_macos(globals())
        else:
            cmd('open', options=dict(layer='overlay', mouseMode='normal', frameless=True, transparent=True, mouseRegions=[dict(x=100, y=100, width=200, height=160)]))
            wait(lambda: cmd('select'))
            ev('await reaper.lifecycle.ready; return true')
            cmd('place'); cmd('focus'); time.sleep(1)
            order()
            ev("await reaper.window.setMouseMode('normal'); inputEvents=[]; return true")
            click(160, 140)
            assert 'pointerdown' in ev('return inputEvents'), dict(baseline=ev('return inputEvents'), state=cmd('state'))
            ev("await reaper.window.setMouseMode('regions'); inputEvents=[]; return true")
            cmd('state')
            click(40, 40)
            state = cmd('state'); events = state['events']
            assert any(e['window'] == 'main' and e['type'] == 1 for e in events), state
            assert not any(e['window'] == 'overlay' for e in events), state
            report.append('Native passthrough click reaches REAPER main window')
            cmd('focus')
            ev('inputEvents=[]; return true')
            mouse('move', 160, 140); mouse('down', 160, 140)
            assert 'pointerdown' in ev('return inputEvents'), dict(input=ev('return inputEvents'), state=cmd('state'))
            ev("await reaper.window.setMouseRegions([]); await reaper.window.setMouseMode('passthrough'); return true")
            assert not ev('return (await reaper.window.getState()).mouseModeActive')
            mouse('drag', 40, 40); mouse('up', 40, 40)
            assert 'pointerup' in ev('return inputEvents'), ev('return inputEvents')
            assert ev('return (await reaper.window.getState()).mouseModeActive')
            report.append('WebView pointer capture and deferred policy survive dragging outside a region')
            cmd('state'); mouse('wheel', 40, 40)
            state = cmd('state'); assert any(e['window'] == 'main' and e['type'] == 22 for e in state['events']), state
            mouse('rightDown', 40, 40); mouse('rightUp', 40, 40)
            cmd('key', key=53, down=True); cmd('key', key=53, down=False); time.sleep(.3)
            state = cmd('state'); assert any(e['window'] == 'main' and e['type'] == 3 for e in state['events']), state
            report.append('Native wheel and right click reach REAPER')
            cmd('focus')
            ev("await reaper.window.setMouseMode('normal'); inputEvents=[]; return true")
            click(40, 40)
            assert 'pointerdown' in ev('return inputEvents')
            report.append('Normal mode receives input on visually transparent pixels')
            ev("await reaper.window.setMouseRegions([{x:100,y:100,width:200,height:160}]); await reaper.window.setMouseMode('regions'); inputEvents=[]; return true")
            cmd('zoom', value=1.5); click(160, 140)
            assert 'pointerdown' in ev('return inputEvents')
            cmd('zoom', value=1)
            report.append('CSS region coordinates follow WKWebView page zoom')
            cmd('floating'); order(True)
            for _ in range(4):
                for target in ('overlay', 'main', 'floating', 'overlay'):
                    cmd('focus', target=target); order(True)
                click(40, 40); order(True)
                click(160, 140); order(True)
            ev('await reaper.window.hide(); return true'); time.sleep(.3)
            assert ev('return (await reaper.window.getState()).layerActive')
            ev('await reaper.window.show(); return true'); order(True)
            cmd('minimize', value=True); time.sleep(.8)
            assert not cmd('state')['visible']
            cmd('minimize', value=False); time.sleep(.8); order(True)
            report.append('Native floating-window ordering, focus, hide/show and minimize/restore')
            cmd('deactivate'); time.sleep(.8)
            state = cmd('state')
            assert not state['active'] and state['overlayNumber'] not in state['stack'], state
            cmd('focus', target='main'); time.sleep(.5); order(True)
            report.append('Switching applications hides the overlay without taking focus back')
            cmd('open', options=dict(layer='overlay', mouseMode='normal', frameless=True))
            wait(lambda: sum(w['webview'] for w in cmd('state')['windows']) == 2)
            for target in ('main', 'overlay', 'floating'):
                cmd('focus', target=target); state = order(True)
                positions = [i for i,w in enumerate(state['windows']) if w['webview']]
                assert all(state['floating'] < i < state['main'] for i in positions), state
            report.append('Two overlays retain the middle layer together')
            cmd('floating', show=False)
            cmd('floating', panel=True)
            cmd('focus', target='main'); order(True)
            cmd('focus'); order(True)
            report.append('Native panels remain above both overlays')
            cmd('floating', show=False)
        (root / 'result.json').write_text(json.dumps(dict(passed=report), indent=2))
        print('PASS: ' + '; '.join(report), flush=True)
    except Exception as error:
        (root / 'result.json').write_text(json.dumps(dict(passed=report, error=str(error)), indent=2))
        raise
    finally:
        try:
            mouse('up', 40, 40)
            cmd('quit')
            process.wait(timeout=10)
        except Exception:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
