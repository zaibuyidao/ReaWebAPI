"""Native Windows/Linux keyboard acceptance in an isolated REAPER profile."""
import argparse, ctypes as C, json, os, shutil, subprocess, sys, time
from pathlib import Path
PAGE = '''<!doctype html><meta charset="utf-8"><title>Keyboard acceptance</title>
<input id="slider" type="range"><input id="text"><textarea id="area"></textarea>
<div id="edit" contenteditable="true"><span>Editable</span></div><button id="button">Button</button>
<div id="shadow"></div><div id="closed"></div><script>
window.keys=[];
for(const type of ['keydown','keyup']) addEventListener(type,e=>keys.push([type,e.code,e.isTrusted]));
window.openInput=document.querySelector('#shadow').attachShadow({mode:'open'}).appendChild(document.createElement('input'));
window.closedInput=document.querySelector('#closed').attachShadow({mode:'closed'}).appendChild(document.createElement('input'));
(async()=>{
  await reaper.lifecycle.ready;
  await reaper.events.on('message', async source=>{
    try { await reaper.host.send(JSON.stringify({value:await (0,eval)(source)})); }
    catch(error) { await reaper.host.send(JSON.stringify({error:error.message})); }
  });
  await reaper.SetExtState('KeyboardProbe','id',String((await reaper.window.getState()).id),false);
})();
</script>'''
LUA = '''local root = ROOT
local id,track,fx,waiting
local started=assert(io.open(root..'/lua-ready','w'));started:write('ready');started:close()
local function reply(value)
  local f=assert(io.open(root..'/reply.tmp','w'));f:write(tostring(value));f:close()
  os.rename(root..'/reply.tmp',root..'/reply.txt')
end
local function tick()
  local ok,err=pcall(function()
    if waiting then
      local text=reaper.ReaWeb_Receive(id)
      if text~='' then waiting=false;reply(text) end
      return
    end
    local f=io.open(root..'/command.txt','r');if not f then return end
    local text=f:read('*a');f:close();os.remove(root..'/command.txt')
    local op,arg=text:match('^([^|]+)|(.*)$')
    if op=='open' then reaper.DeleteExtState('KeyboardProbe','id',false);id=reaper.ReaWeb_Open(root..'/Scripts/Keyboard/index.html');assert(id>0,reaper.ReaWeb_GetLastError());reply(id)
    elseif op=='jsfx' then
      if not track then
        reaper.InsertTrackAtIndex(0,false);track=reaper.GetTrack(0,0)
        fx=reaper.TrackFX_AddByName(track,'keyboard/probe.jsfx',false,-1);assert(fx>=0,'Missing probe JSFX')
      end
      reaper.TrackFX_Show(track,fx,tonumber(arg));reply(true)
    elseif op=='ready' then id=tonumber(reaper.GetExtState('KeyboardProbe','id'));reply(id or 0)
    elseif op=='eval' then assert(reaper.ReaWeb_Send(id,arg));waiting=true
    elseif op=='focus' then reply(reaper.ReaWeb_Focus(id))
    elseif op=='cursor' then reaper.SetEditCurPos(tonumber(arg),false,false);reply(reaper.GetCursorPosition())
    elseif op=='state' then reply(reaper.GetCursorPosition())
    elseif op=='closefx' then reaper.TrackFX_Show(track,fx,2);reaper.TrackFX_Show(track,fx,0);reply(true)
    elseif op=='dock' then reply(reaper.ReaWeb_SetDocked(id,arg=='true'))
    end
  end)
  if not ok then reply('ERROR:'..tostring(err)) end
  reaper.defer(tick)
end
tick()
'''
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ['reaper','extension','output']: parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--app-only',action='store_true')
    parser.add_argument('--overlay',action='store_true',help='Exercise the SVG overlay example and Main shortcut routing')
    args=parser.parse_args(); root=args.output.resolve();root.mkdir(parents=True,exist_ok=False)
    (root/'UserPlugins').mkdir();shutil.copy2(args.extension,root/'UserPlugins'/args.extension.name)
    for helper in args.extension.parent.glob('reawebapi-webview-*'):
        target=root/'UserPlugins'/helper.name;shutil.copy2(helper,target);target.chmod(0o755)
    for name in ['reaper-install-rev.txt','reaper-license.rk']:
        source=args.reaper.resolve().parent/name
        if source.is_file():shutil.copy2(source,root/name)
    config='[REAPER]\nloadlastproj=0\nshowlastproj=0\nsplash=0\nverchk=0\nvst_scan=0\nhasrecentlyopened=1\nerrnowarn=5\nnewprojdo=0\n'
    config+=('[audioconfig]\nmode=4\nwaveout_devicein=-1\nwaveout_deviceout=0\n' if sys.platform=='win32' else 'audiomode=4\n')
    (root/'EmptyVST').mkdir()
    config=config.replace('vst_scan=0\n','vst_scan=0\nvstfullstate=49989\nvstpath64='+str(root/'EmptyVST')+'\n')
    if sys.platform=='win32': config+='waveout_srate=48000\nwaveout_bps=16\nwaveout_bs=256\nwaveout_numblocks=4\nwaveout_nch_out=2\n'
    if sys.platform!='win32':config=config.replace('[REAPER]','[reaper]')
    config+='\nvstpath='+str(root/'EmptyVST')+'\n' if sys.platform!='win32' else ''
    (root/'reaper.ini').write_text(config)
    # A custom Space action makes hard-coded Play/Stop routing fail this test.
    (root/'reaper-kb.ini').write_text('KEY 1 32 40042 0\n')
    if args.overlay:
        (root/'reaper-kb.ini').write_text(''.join(f'KEY {flags} {key} 40042 0\n' for flags,key in [(1,32),(1,65),(5,65),(9,65),(13,65),(17,65),(1,37),(1,119)]))
    for folder in ['Scripts/Keyboard','Effects/keyboard']:
        path=root/folder;path.mkdir(parents=True);(path/'index.html').write_text(PAGE,encoding='utf-8')
    (root/'Scripts/Keyboard/app.json').write_text('{"id":"keyboard-acceptance"}')
    if args.overlay:
        example=Path(__file__).resolve().parents[1]/'examples/windows/overlay'
        for source in example.iterdir():
            if source.suffix in ['.html','.js','.css']:shutil.copy2(source,root/'Scripts/Keyboard'/source.name)
        with (root/'Scripts/Keyboard/index.html').open('a',encoding='utf-8') as page:
            page.write(PAGE[PAGE.index('<script>'):].replace("window.openInput=", "document.body.insertAdjacentHTML('beforeend','<div id=shadow></div><div id=closed></div>');window.openInput="))
    effect=root/'Effects/keyboard'
    shutil.copy2(Path(__file__).resolve().parents[1]/'runtime/jsfx-webui.mjs',effect/'jsfx-webui.mjs')
    with (effect/'index.html').open('a') as page:
        page.write("<script type=\"module\">import {connectJSFX} from './jsfx-webui.mjs';window.jsfx=await connectJSFX(reaper);</script>")
    (effect/'probe.jsfx').write_text('desc:Keyboard acceptance\nslider1:0.5<0,1,0.01>Gain\n@sample\nspl0*=slider1;spl1*=slider1;\n')
    (root/'launch.lua').write_text(LUA.replace('ROOT',json.dumps(root.as_posix())),encoding='utf-8')
    if args.overlay:
        launcher=root/'launch.lua'
        launcher.write_text(launcher.read_text().replace("reaper.ReaWeb_Open(root..'/Scripts/Keyboard/index.html')", "reaper.ReaWeb_OpenEx(root..'/Scripts/Keyboard/index.html','{\"frameless\":true,\"transparent\":true,\"layer\":\"overlay\",\"mouseMode\":\"regions\"}')"))
    if sys.platform=='win32':
        from ctypes import wintypes as W
        user=C.WinDLL('user32');user.keybd_event.argtypes=[C.c_ubyte,C.c_ubyte,C.c_uint32,C.c_size_t]
        user.GetForegroundWindow.restype=W.HWND
        user.GetWindowThreadProcessId.argtypes=[W.HWND,C.POINTER(W.DWORD)]
        user.GetAncestor.argtypes=[W.HWND,W.UINT];user.GetAncestor.restype=W.HWND
        user.SetForegroundWindow.argtypes=[W.HWND]
        user.IsWindowVisible.argtypes=[W.HWND]
        user.GetClassNameW.argtypes=[W.HWND,W.LPWSTR,C.c_int]
        def activate():
            targets=[];fallback=[];mains=[]
            @C.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
            def child(hwnd,_):
                name=C.create_unicode_buffer(256);user.GetClassNameW(hwnd,name,256)
                if name.value=='REAPERwnd':mains.append(hwnd)
                if name.value=='ReaWebAPI.Window' and user.IsWindowVisible(hwnd):targets.append(user.GetAncestor(hwnd,2))
                return True
            @C.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
            def top(hwnd,_):
                pid=W.DWORD();user.GetWindowThreadProcessId(hwnd,C.byref(pid))
                if pid.value==process.pid:
                    if user.IsWindowVisible(hwnd):fallback.append(hwnd)
                    child(hwnd,0);user.EnumChildWindows(W.HWND(hwnd),child,0)
                return True
            user.EnumWindows(top,0);targets=targets or fallback;assert targets,'No visible test window'
            current=C.WinDLL('kernel32').GetCurrentThreadId()
            threads={user.GetWindowThreadProcessId(user.GetForegroundWindow(),None),user.GetWindowThreadProcessId(targets[0],None)}-{current,0}
            for thread in threads:user.AttachThreadInput(current,thread,True)
            try:
                if args.overlay and mains:user.SetForegroundWindow(mains[0]);time.sleep(.1)
                user.SetForegroundWindow(targets[0]);time.sleep(.1)
                assert user.GetForegroundWindow()==targets[0]
            finally:
                for thread in threads:user.AttachThreadInput(current,thread,False)
        def native_key(key,down):user.keybd_event(key,user.MapVirtualKeyW(key,0),(0 if down else 2)|(1 if key in range(33,41) else 0),0)
        codes=dict(space=32,enter=13,shift=16,ctrl=17,alt=18,a=65,left=37,f8=119)
    else:
        def activate(): pass
        x11=C.CDLL('libX11.so.6');xtst=C.CDLL('libXtst.so.6');x11.XOpenDisplay.restype=C.c_void_p
        display=x11.XOpenDisplay(None);assert display,'No X11 display'
        x11.XKeysymToKeycode.argtypes=[C.c_void_p,C.c_ulong];x11.XKeysymToKeycode.restype=C.c_ubyte
        x11.XFlush.argtypes=[C.c_void_p];xtst.XTestFakeKeyEvent.argtypes=[C.c_void_p,C.c_uint,C.c_int,C.c_ulong]
        def native_key(key,down):
            xtst.XTestFakeKeyEvent(display,x11.XKeysymToKeycode(display,key),down,0);x11.XFlush(display)
        codes=dict(space=32,enter=0xff0d,shift=0xffe1,ctrl=0xffe3,alt=0xffe9,a=97,left=0xff51,f8=0xffc5)
        x11.XGetInputFocus.argtypes=[C.c_void_p,C.POINTER(C.c_ulong),C.POINTER(C.c_int)]
        def native_focus():
            window=C.c_ulong();revert=C.c_int();x11.XGetInputFocus(display,C.byref(window),C.byref(revert))
            return hex(window.value)
    def wait(fn,timeout=25):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            if process.poll() is not None: raise RuntimeError("REAPER exited: "+str(process.returncode))
            value=fn()
            if value:return value
            time.sleep(.08)
        raise AssertionError('Timed out')
    def command(op,arg=''):
        (root/'reply.txt').unlink(missing_ok=True)
        (root/'command.tmp').write_text(op+'|'+str(arg),encoding='utf-8');(root/'command.tmp').replace(root/'command.txt')
        def read_reply():
            try:return (root/'reply.txt').read_text(encoding='utf-8')
            except (FileNotFoundError,PermissionError):return None
        value=wait(read_reply)
        assert not value.startswith('ERROR:'),value
        return value
    def evaluate(script):
        result=json.loads(command('eval',script));assert 'error' not in result,result
        return result.get('value')
    def press(name,shift=False):
        activate()
        assert evaluate('document.hasFocus()'), 'Native input requires an active WebView'
        if shift:native_key(codes['shift'],True)
        native_key(codes[name],True);time.sleep(.08);native_key(codes[name],False)
        if shift:native_key(codes['shift'],False)
        time.sleep(.3)
    def reload_page(jsfx=False):
        evaluate('reaper.DeleteExtState("KeyboardProbe","id",false)')
        evaluate('setTimeout(()=>location.reload(),800);true');time.sleep(1)
        wait(lambda:int(command('ready'))>0)
        if jsfx:wait(lambda:evaluate('!!window.jsfx'))
        activate();command('focus');time.sleep(.3)
    report=[]
    with (root/'process.log').open('w') as log:
        process=subprocess.Popen([str(args.reaper.resolve()),'-newinst','-cfgfile',str(root/'reaper.ini'),'-new','-ignoreerrors','-nosplash',str(root/'launch.lua')],stdout=log,stderr=log)
        try:
            wait(lambda:(root/'lua-ready').exists(),120)
            if args.overlay:
                if sys.platform.startswith('linux'):
                    time.sleep(12)
                    about=subprocess.run(['xdotool','search','--onlyvisible','--pid',str(process.pid),'--name','About REAPER'],capture_output=True,text=True)
                    if about.returncode==0:
                        for hwnd in about.stdout.split():
                            geometry=dict(line.split('=',1) for line in subprocess.check_output(['xdotool','getwindowgeometry','--shell',hwnd],text=True).splitlines() if '=' in line)
                            subprocess.run(['xdotool','mousemove','--sync',str(int(geometry['X'])+int(geometry['WIDTH'])-85),str(int(geometry['Y'])+int(geometry['HEIGHT'])-27),'click','1'],check=True)
                        time.sleep(.5)
                from overlay_acceptance import run
                run(locals())
                return
            for mode in ([] if args.app_only else ['default',True,False]):
                ui=dict(entry='index.html',width=640,height=420)
                if mode!='default':ui['dawKeyboard']=mode
                (effect/'probe.json').write_text(json.dumps(dict(schema=1,JSFX_WebUI=ui)))
                for floating in [False,True]:
                    command('jsfx',3 if floating else 1);time.sleep(.8);activate();wait(lambda:int(command('ready'))>0)
                    wait(lambda:evaluate('!!window.jsfx'))
                    activate();command('focus');time.sleep(.4)
                    wait(lambda:evaluate('document.hasFocus()'))
                    evaluate("document.querySelector('#text').focus();keys=[];true");press('enter')
                    assert len(evaluate('keys'))==2, 'Native input did not reach the page'
                    for target in ["document.querySelector('#slider')","document.querySelector('#button')","document.querySelector('#text')","document.querySelector('#area')","document.querySelector('#edit')",'openInput','closedInput']:
                        editable=target not in ["document.querySelector('#slider')","document.querySelector('#button')"]
                        evaluate(f'{target}.focus();keys=[];true');command('cursor',12);press('space')
                        host=mode is not False and not editable;position=float(command('state'));keys=evaluate('keys')
                        assert position==(0 if host else 12),(mode,floating,target,position,keys)
                        assert len(keys)==(0 if host else 2),(mode,target,keys)
                    evaluate("document.querySelector('#slider').focus();keys=[];true");command('cursor',12);press('space',shift=True)
                    assert float(command('state'))==12
                    if mode=='default' and not floating:
                        evaluate("document.querySelector('#slider').focus();keys=[];true")
                        native_key(codes['space'],True);time.sleep(.8)
                        command('cursor',12);native_key(codes['space'],True);time.sleep(.2)
                        repeat_position=float(command('state'))
                        native_key(codes['shift'],True);command('cursor',12)
                        native_key(codes['space'],True);time.sleep(.2)
                        native_key(codes['space'],False);native_key(codes['shift'],False);time.sleep(.2)
                        assert float(command('state'))==repeat_position, ('Held Space changed shortcut ownership after Shift',repeat_position)
                    report.append(dict(mode=mode,floating=floating,modifiedKeys=evaluate('keys'),passed=True))
                    initial=mode is not False
                    before=(effect/'probe.json').read_bytes()
                    for enabled in [not initial,initial]:
                        assert evaluate('reaper.window.setDawKeyboard('+str(enabled).lower()+')')==enabled
                        for target,editable in [("document.querySelector('#button')",False),('closedInput',True)]:
                            evaluate(f'{target}.focus();keys=[];true');command('cursor',12);press('space')
                            host=enabled and not editable
                            assert float(command('state'))==(0 if host else 12)
                            assert len(evaluate('keys'))==(0 if host else 2)
                    evaluate('reaper.window.setDawKeyboard('+str(not initial).lower()+')')
                    reload_page(True)
                    assert evaluate('reaper.window.getState().then(s=>s.dawKeyboard)')==initial
                    assert (effect/'probe.json').read_bytes()==before
                    report.append(dict(jsfxDynamic=True,mode=mode,floating=floating,reload=initial,passed=True))
                    command('closefx');time.sleep(.6)
            command('open');wait(lambda:int(command('ready'))>0);command('focus');time.sleep(.3)
            assert evaluate('reaper.window.getState().then(s=>s.dawKeyboard)') is False
            manifest=root/'Scripts/Keyboard/app.json'
            for initial in [True,False,None]:
                config={'id':'keyboard-acceptance'}
                if initial is not None:config['dawKeyboard']=initial
                manifest.write_text(json.dumps(config));before=manifest.read_bytes()
                reload_page();expected=initial is True
                assert evaluate('reaper.window.getState().then(s=>s.dawKeyboard)')==expected
                evaluate("document.querySelector('#button').focus();keys=[];true");command('cursor',12);press('space')
                assert float(command('state'))==(0 if expected else 12)
                evaluate('reaper.window.setDawKeyboard('+str(not expected).lower()+')')
                assert manifest.read_bytes()==before
                reload_page()
                assert evaluate('reaper.window.getState().then(s=>s.dawKeyboard)')==expected
                report.append(dict(appConfig=initial,reload=expected,passed=True))
            for docked in [False,True]:
                command('dock',str(docked).lower());command('focus');time.sleep(.3)
                for capture in [True,False]:
                    evaluate('reaper.window.setKeyboardCapture('+str(capture).lower()+')')
                    baseline=[]
                    for enabled in [False,True,False]:
                        evaluate('reaper.window.setDawKeyboard('+str(enabled).lower()+')')
                        state=evaluate('reaper.window.getState()')
                        assert state['dawKeyboard']==enabled and state['keyboardCapture']==capture,state
                        actual=[]
                        for target in ["document.querySelector('#slider')","document.querySelector('#button')","document.querySelector('#text')","document.querySelector('#area')","document.querySelector('#edit')",'openInput','closedInput']:
                            editable=target not in ["document.querySelector('#slider')","document.querySelector('#button')"]
                            evaluate(f'{target}.focus();keys=[];true');command('cursor',12);press('space')
                            position=float(command('state'));keys=evaluate('keys');actual.append([position,keys])
                            if enabled:
                                assert position==(12 if editable else 0),(docked,capture,target,position,keys)
                                assert len(keys)==(2 if editable else 0),(docked,capture,target,keys)
                        if not enabled:
                            if baseline:assert actual==baseline,(docked,capture,actual,baseline)
                            else:baseline=actual
                        report.append(dict(appRouting=True,docked=docked,capture=capture,enabled=enabled,controls=actual,passed=True))
            evaluate('reaper.window.setKeyboardCapture(true)')
            for docked in [False,True,False,True,False]:
                command('dock',str(docked).lower());command('focus');time.sleep(.3)
                evaluate("document.querySelector('#text').focus();keys=[];true")
                for _ in range(3):press('enter')
                actual=dict(keys=evaluate('keys'),focused=evaluate('document.hasFocus()'))
                if sys.platform.startswith('linux'):actual['nativeFocus']=native_focus()
                expected=[['keydown','Enter',True],['keyup','Enter',True]]*3
                assert actual['keys']==expected and actual['focused'], actual
                evaluate('keys=[];true');press('a');press('space');press('enter',shift=True)
                keys=evaluate('keys')
                for code in ['KeyA','Space','Enter']:
                    assert [row for row in keys if row[1]==code]==[['keydown',code,True],['keyup',code,True]],keys
                assert evaluate('document.hasFocus()')
                report.append(dict(app=True,docked=docked,enterPairs=3,otherKeys=True,passed=True))
        finally:
            if sys.platform=='win32' and args.overlay:
                try:
                    from PIL import ImageGrab
                    ImageGrab.grab(all_screens=True).save(root/'desktop.png')
                except Exception:pass
            if sys.platform.startswith('linux'):
                try:
                    import gi
                    gi.require_version('Gdk','3.0')
                    from gi.repository import Gdk
                    screen=Gdk.get_default_root_window()
                    Gdk.pixbuf_get_from_window(screen,0,0,screen.get_width(),screen.get_height()).savev(str(root/'desktop.png'),'png',[],[])
                except Exception: pass
            (root/'result.json').write_text(json.dumps(report,indent=2));process.terminate();process.wait(timeout=10)
            (root/'reaper-license.rk').unlink(missing_ok=True)
    print(json.dumps(report),flush=True)
if __name__=='__main__':main()
