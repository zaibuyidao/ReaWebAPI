"""Native acceptance of the shipped SVG example, called by keyboard_reaper --overlay."""
import ctypes as C
import sys
import time

def opacity_control(ev, mouse):
    ev('reaper.window.focus()');time.sleep(.3)
    rect=ev('(()=>{const r=document.querySelector("#opacity").getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height}})()')
    x,y=rect['x']+rect['width']*.75,rect['y']+rect['height']/2
    mouse('move',x,y);mouse('down',x,y);mouse('drag',rect['x']+rect['width']*.4,y);mouse('up',rect['x']+rect['width']*.4,y)
    value=ev('Number(document.querySelector("#opacity").value)')
    assert 20<value<70,(value,rect,ev('reaper.window.getPosition()'),ev('reaper.window.getSize()'),ev('reaper.system.getDisplays()'),ev('window.pointerLog'))
    assert ev('getComputedStyle(document.querySelector("main")).opacity')=='1'
    assert ev('getComputedStyle(document.querySelector(".decoration")).opacity')=='1'
    assert abs(float(ev('document.querySelector("main").style.getPropertyValue("--panel-opacity")'))-value/100)<.001
    ev('(()=>{const input=document.querySelector("#opacity");input.value=100;input.dispatchEvent(new Event("input"));return true})()')

def window_mode(ev, mouse, wait, refresh=lambda: None):
    ev("(document.querySelector('[data-language=\"zh-CN\"]').click(),true)")
    assert ev('document.documentElement.lang')=='zh-CN'
    ev("(document.querySelector('[data-language=\"en\"]').click(),true)")
    ev("(()=>{document.querySelector('#mode').value='normal';document.querySelector('#mode').dispatchEvent(new Event('change'));return true})()")
    wait(lambda:ev('document.body.hasAttribute("data-reaweb-drag")'))
    assert ev('getComputedStyle(document.body).backgroundColor')=='rgba(0, 0, 0, 0)'
    ev('(async()=>{await reaper.window.setSize(900,650);await reaper.window.setPosition(80,80);return true})()')
    ev('reaper.window.focus()')
    time.sleep(.5);refresh()
    before=ev('reaper.window.getPosition()')
    mouse('move',100,400);mouse('down',100,400);mouse('drag',150,440);mouse('up',150,440)
    after=ev('reaper.window.getPosition()')
    assert abs(after['x']-before['x'])>=30 and abs(after['y']-before['y'])>=20,(before,after,ev('window.dragLayout'),ev('reaper.window.getState()'),ev('window.pointerLog'))
    refresh();size=ev('reaper.window.getSize()');width=ev('innerWidth');height=ev('innerHeight')
    mouse('move',width-1,height/2);mouse('down',width-1,height/2)
    for step in range(1,9):mouse('drag',width-1+step*10,height/2)
    mouse('up',width+79,height/2)
    resized=ev('reaper.window.getSize()')
    assert resized['width']>=size['width']+40,(size,resized)
    ev("(()=>{document.querySelector('#mode').value='regions';document.querySelector('#mode').dispatchEvent(new Event('change'));return true})()")
    wait(lambda:ev('!document.body.hasAttribute("data-reaweb-drag")'))
    ev("document.querySelector('#fit').onclick()");time.sleep(.5);refresh()

def run(context):
    ev,cmd,wait,activate,key,codes,report=[context[k] for k in ('evaluate','command','wait','activate','native_key','codes','report')]
    cmd('open');wait(lambda:int(cmd('ready'))>0)
    wait(lambda:ev('document.querySelector("#pan").hasAttribute("aria-valuenow")'))
    wait(lambda:ev('reaper.window.getState().then(s=>s.shortcutRouting && s.mouseModeActive)'))
    state=ev('reaper.window.getState()');assert state['layer']=='overlay' and state['layerActive'],state
    activate();cmd('focus');wait(lambda:ev('document.hasFocus()'))
    ev("window.shortcutPackets=[];const stringify=JSON.stringify;JSON.stringify=function(value,...args){if(value?.__reawebDragRegions)window.dragLayout=value.__reawebDragRegions;if(value && typeof value.down==='boolean' && value.document)shortcutPackets.push({...value});return stringify.call(this,value,...args)};true")
    for name,mods in [('space',[]),('a',[]),('a',['shift']),('a',['ctrl']),('a',['ctrl','shift']),('a',['alt']),('left',[]),('f8',[])]:
        activate();cmd('focus');wait(lambda:ev('document.hasFocus()'));ev("document.querySelector('#pan').focus();keys=[];true");cmd('cursor',12)
        for mod in mods:key(codes[mod],True);time.sleep(.04)
        key(codes[name],True);time.sleep(.1);key(codes[name],False)
        for mod in reversed(mods):key(codes[mod],False);time.sleep(.04)
        time.sleep(.3)
        assert float(cmd('state'))==0,(name,mods,cmd('state'),ev('keys'),ev('shortcutPackets'))
        report.append(dict(shortcut=name,modifiers=mods,passed=True))
    for target in ["document.querySelector('#note')",'openInput','closedInput']:
        activate();cmd('focus');wait(lambda:ev('document.hasFocus()'));ev(f'{target}.focus();{target}.value="";true');cmd('cursor',12)
        key(codes['a'],True);key(codes['a'],False);time.sleep(.3)
        assert float(cmd('state'))==12 and ev(f'{target}.value')=='a',(target,ev('keys'))
    report.append(dict(textAndShadowInputs=True,passed=True))
    ev("document.activeElement.blur();window.pointerLog=[];for(const t of ['pointerdown','pointerup','pointermove'])addEventListener(t,e=>pointerLog.push([t,e.isTrusted]));true")
    position=ev('reaper.window.getPosition()');size=ev('reaper.window.getSize()');width=ev('innerWidth');scale=size['width']/width
    if sys.platform=='win32':
        user=context['user'];user.SetCursorPos.argtypes=[C.c_int,C.c_int]
        from ctypes import wintypes as W
        user.WindowFromPoint.argtypes=[W.POINT];user.WindowFromPoint.restype=W.HWND
        window_hits=[]
        def cursor_position():
            point=W.POINT();user.GetCursorPos(C.byref(point));return point.x,point.y
        def mouse(kind,x,y):
            sx,sy=round(position['x']+x*scale),round(position['y']+y*scale)
            left,top,width,height=[user.GetSystemMetrics(index) for index in (76,77,78,79)]
            user.mouse_event(0xc001,round((sx-left)*65535/(width-1)),round((sy-top)*65535/(height-1)),0,0)
            time.sleep(.08)
            point=W.POINT();user.GetCursorPos(C.byref(point));hit=user.WindowFromPoint(point);kind_name=C.create_unicode_buffer(128);user.GetClassNameW(hit,kind_name,128);window_hits.append((kind,point.x,point.y,kind_name.value,user.GetForegroundWindow()))
            if kind not in ('move','drag'):user.mouse_event({'down':2,'up':4,'wheel':0x800}[kind],0,0,120 if kind=='wheel' else 0,0)
            time.sleep(.15)
    else:
        x11,xtst,display=[context[k] for k in ('x11','xtst','display')]
        xtst.XTestFakeMotionEvent.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_int,C.c_ulong]
        xtst.XTestFakeButtonEvent.argtypes=[C.c_void_p,C.c_uint,C.c_int,C.c_ulong]
        xtst.XTestFakeRelativeMotionEvent.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_ulong]
        last_mouse=[0,0]
        x11.XDefaultRootWindow.argtypes=[C.c_void_p];x11.XDefaultRootWindow.restype=C.c_ulong
        x11.XQueryPointer.argtypes=[C.c_void_p,C.c_ulong,C.POINTER(C.c_ulong),C.POINTER(C.c_ulong),C.POINTER(C.c_int),C.POINTER(C.c_int),C.POINTER(C.c_int),C.POINTER(C.c_int),C.POINTER(C.c_uint)]
        def cursor_position():
            root,child=C.c_ulong(),C.c_ulong();rx,ry,wx,wy=(C.c_int() for _ in range(4));mask=C.c_uint()
            x11.XQueryPointer(display,x11.XDefaultRootWindow(display),C.byref(root),C.byref(child),C.byref(rx),C.byref(ry),C.byref(wx),C.byref(wy),C.byref(mask))
            return rx.value,ry.value
        def mouse(kind,x,y):
            if ev('document.pointerLockElement !== null'):
                xtst.XTestFakeRelativeMotionEvent(display,round((x-last_mouse[0])*scale),round((y-last_mouse[1])*scale),0)
            else:
                xtst.XTestFakeMotionEvent(display,-1,round(position['x']+x*scale),round(position['y']+y*scale),0)
            last_mouse[:]=[x,y]
            if kind=='wheel':
                xtst.XTestFakeButtonEvent(display,4,True,0);xtst.XTestFakeButtonEvent(display,4,False,0)
            elif kind not in ('move','drag'):xtst.XTestFakeButtonEvent(display,1,kind=='down',0)
            x11.XFlush(display);time.sleep(.15)
    rect=ev('(()=>{const r=document.querySelector("#pan").getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height}})()')
    x,y=rect['x']+rect['width']/2,rect['y']+rect['height']/2
    activate();cmd('focus');time.sleep(.3)
    before=ev('reaper.GetMasterTrack(0).then(t=>reaper.GetMediaTrackInfo_Value(t,"D_PAN"))')
    ev("window.gestureLog=[];for(const t of ['mousemove','pointermove','pointerlockchange','blur'])addEventListener(t,e=>gestureLog.push([t,e.movementY,document.pointerLockElement?.id]));true")
    mouse('move',x,y);origin=cursor_position();mouse('down',x,y)
    wait(lambda:ev('document.pointerLockElement?.id === "pan"'))
    mouse('move',x-180,y-20);mouse('up',x-180,y-20)
    wait(lambda:ev('document.pointerLockElement === null'))
    restored=cursor_position();assert all(abs(a-b)<=1 for a,b in zip(origin,restored)),(origin,restored)
    after=ev('reaper.GetMasterTrack(0).then(t=>reaper.GetMediaTrackInfo_Value(t,"D_PAN"))')
    assert before!=after and ['pointerup',True] in ev('pointerLog'),(before,after,ev('pointerLog'),ev('gestureLog'),rect,position,size,width,ev('reaper.system.getDisplays()'),window_hits if sys.platform=='win32' else [])
    report.append(dict(svgDragOutside=True,actualMasterPanChanged=True,pointerLockAndCursorRestored=True,passed=True))
    activate();cmd('focus');ev('pointerLog=[];true');mouse('down',rect['x']+1,rect['y']+1);mouse('up',rect['x']+1,rect['y']+1)
    assert not any(row[0]=='pointerdown' for row in ev('pointerLog')),ev('pointerLog')
    report.append(dict(svgBoundingCornerPassesThrough=True,passed=True))
    activate();cmd('focus');ev("reaper.window.setMouseMode('passthrough')");ev('pointerLog=[];true')
    mouse('down',x,y);mouse('move',x-100,y);mouse('up',x-100,y);mouse('wheel',x,y)
    assert not any(row[0]=='pointerdown' for row in ev('pointerLog')),ev('pointerLog')
    ev("reaper.window.setMouseMode('regions')")
    report.append(dict(fullPassthroughAndRecovery=True,passed=True))
    opacity_control(ev,mouse)
    report.append(dict(regionsModeBackgroundOpacity=True,passed=True))
    def refresh():
        nonlocal position,scale
        position=ev('reaper.window.getPosition()');scale=ev('reaper.window.getSize()')['width']/ev('innerWidth')
    window_mode(ev,mouse,wait,refresh)
    rect=ev('(()=>{const r=document.querySelector("#pan").getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height}})()')
    x,y=rect['x']+rect['width']/2,rect['y']+rect['height']/2
    report.append(dict(languageSwitching=True,transparentNativeMoveResize=True,passed=True))
    if sys.platform=='win32':
        from ctypes import wintypes as W
        user.GetWindowTextW.argtypes=[W.HWND,W.LPWSTR,C.c_int]
        def stacking(label):
            rows=[]
            @C.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
            def collect(hwnd,_):
                pid=W.DWORD();user.GetWindowThreadProcessId(hwnd,C.byref(pid))
                if pid.value==context['process'].pid and user.IsWindowVisible(hwnd):
                    title=C.create_unicode_buffer(512);kind=C.create_unicode_buffer(128)
                    user.GetWindowTextW(hwnd,title,512);user.GetClassNameW(hwnd,kind,128)
                    rows.append((hwnd,title.value,kind.value,user.GetWindowLongW(hwnd,-20),user.GetWindow(hwnd,4)))
                return True
            user.GetTopWindow.argtypes=[W.HWND];user.GetTopWindow.restype=W.HWND
            user.GetWindow.argtypes=[W.HWND,W.UINT];user.GetWindow.restype=W.HWND
            hwnd=user.GetTopWindow(None)
            while hwnd:
                collect(hwnd,0);hwnd=user.GetWindow(hwnd,2)
            overlay=next(i for i,row in enumerate(rows) if row[2]=='ReaWebAPI.Window')
            main=next(i for i,row in enumerate(rows) if row[2]=='REAPERwnd')
            floating=[i for i,row in enumerate(rows) if label in row[1]]
            assert floating and all(i<overlay<main for i in floating),(rows,ev('reaper.window.getState()'))
            return rows[main][0]
        cmd('jsfx',3);time.sleep(.6)
        for _ in range(2):
            activate();mouse('down',x,y);mouse('up',x,y);time.sleep(.3)
            main=stacking('Keyboard acceptance');user.SetForegroundWindow(main);time.sleep(.3);stacking('Keyboard acceptance')
        report.append(dict(realFloatingFXAndMainActivation=True,passed=True))
        cmd('closefx');ev('(async()=>{const t=await reaper.GetTrack(0,0),item=await reaper.CreateNewMIDIItemInProj(t,0,4,false);await reaper.SetMediaItemSelected(item,true);await reaper.Main_OnCommand(40153,0);return true})()');time.sleep(.5)
        activate();mouse('down',x,y);mouse('up',x,y);time.sleep(.3);main=stacking('MIDI')
        user.SetForegroundWindow(main);time.sleep(.3);stacking('MIDI')
        report.append(dict(realFloatingMidiAndMainActivation=True,passed=True))
        assert 'toolbar 1' in ev('reaper.kbd_getTextFromCmd(41679,0)').lower()
        ev('reaper.Main_OnCommand(41679,0)');time.sleep(.5)
        activate();mouse('down',x,y);mouse('up',x,y);time.sleep(.3);main=stacking('oolbar')
        user.SetForegroundWindow(main);time.sleep(.3);stacking('oolbar')
        report.append(dict(realFloatingToolbarAndMainActivation=True,passed=True))

    else:
        import subprocess,re
        def stacking(label):
            ids=re.findall(r'0x[0-9a-f]+',subprocess.check_output(['xprop','-root','_NET_CLIENT_LIST_STACKING'],text=True))
            rows=[(wid,subprocess.check_output(['xprop','-id',wid,'_NET_WM_NAME','WM_TRANSIENT_FOR','_NET_WM_STATE','_NET_WM_WINDOW_TYPE'],text=True)) for wid in ids]
            overlay=next(i for i,row in enumerate(rows) if 'REAPER SVG Overlay' in row[1])
            main=next(i for i,row in enumerate(rows) if 'REAPER v' in row[1] and 'About REAPER' not in row[1])
            floating=[i for i,row in enumerate(rows) if label in row[1]]
            assert floating and all(main<overlay<i for i in floating),(rows,ev('reaper.window.getState()'),subprocess.check_output(['xwininfo','-root','-tree'],text=True))
            return rows[main][0]
        cmd('jsfx',3);time.sleep(.7)
        cmd('focus');time.sleep(.3);main=stacking('Keyboard acceptance')
        subprocess.run(['xdotool','windowactivate','--sync',main],check=True);time.sleep(.4);stacking('Keyboard acceptance')
        report.append(dict(realFloatingFXAndMainActivation=True,passed=True))
        cmd('closefx');ev('(async()=>{const t=await reaper.GetTrack(0,0),item=await reaper.CreateNewMIDIItemInProj(t,0,4,false);await reaper.SetMediaItemSelected(item,true);await reaper.Main_OnCommand(40153,0);return true})()');time.sleep(.7)
        cmd('focus');time.sleep(.3);main=stacking('MIDI')
        subprocess.run(['xdotool','windowactivate','--sync',main],check=True);time.sleep(.4);stacking('MIDI')
        report.append(dict(realFloatingMidiAndMainActivation=True,passed=True))
        assert 'toolbar 1' in ev('reaper.kbd_getTextFromCmd(41679,0)').lower()
        ev('reaper.Main_OnCommand(41679,0)');time.sleep(.7)
        cmd('focus');time.sleep(.3);main=stacking('oolbar')
        subprocess.run(['xdotool','windowactivate','--sync',main],check=True);time.sleep(.4);stacking('oolbar')
        report.append(dict(realFloatingToolbarAndMainActivation=True,passed=True))


def run_macos(context):
    cmd,raw,wait,mouse,click,report=[context[k] for k in ('cmd','ev','wait','mouse','click','report')]
    def ev(source):return raw('return await ('+source+');')
    cmd('open',options=dict(layer='overlay',mouseMode='regions',frameless=True,transparent=True))
    wait(lambda:cmd('select'))
    cmd('fit-main')
    wait(lambda:ev('document.querySelector("#pan").hasAttribute("aria-valuenow")'))
    wait(lambda:ev('reaper.window.getState().then(s=>s.shortcutRouting && s.mouseModeActive)'))
    cmd('focus');time.sleep(.5)
    for name,key,flags in [('space',49,0),('a',0,0),('shift-a',0,1<<17),('command-a',0,1<<20),('command-shift-a',0,(1<<20)|(1<<17)),('option-a',0,1<<19),('left',123,0),('f8',100,0)]:
        cmd('focus');time.sleep(.15)
        raw("document.querySelector('#pan').focus(); await reaper.SetEditCurPos(12,false,false);return true")
        cmd('key',key=key,flags=flags,down=True);time.sleep(.1);cmd('key',key=key,flags=flags,down=False);time.sleep(.3)
        assert ev('reaper.GetCursorPosition()')==0,(name,ev('reaper.GetCursorPosition()'),cmd('state'))
        report.append('Main shortcut '+name)
    cmd('focus');time.sleep(.15)
    raw("document.querySelector('#note').focus();document.querySelector('#note').value='';await reaper.SetEditCurPos(12,false,false);return true")
    cmd('key',key=0,down=True);cmd('key',key=0,down=False);time.sleep(.3)
    assert ev('reaper.GetCursorPosition()')==12 and ev('document.querySelector("#note").value')=='a'
    report.append('Text input retains focus and does not trigger Main shortcuts')
    raw("document.activeElement.blur();window.pointerLog=[];for(const type of ['pointerdown','pointerup'])addEventListener(type,e=>pointerLog.push([type,e.isTrusted]));return true")
    rect=ev('(()=>{const r=document.querySelector("#pan").getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height}})()')
    x,y=rect['x']+rect['width']/2,rect['y']+rect['height']/2
    before=ev('reaper.GetMasterTrack(0).then(t=>reaper.GetMediaTrackInfo_Value(t,"D_PAN"))')
    mouse('move',x,y);origin=context['cursor_position']();mouse('down',x,y)
    wait(lambda:ev('document.pointerLockElement?.id === "pan"'))
    mouse('drag',x-180,y-20);mouse('up',x-180,y-20)
    wait(lambda:ev('document.pointerLockElement === null'))
    restored=context['cursor_position']();assert all(abs(a-b)<=1 for a,b in zip(origin,restored)),(origin,restored)
    after=ev('reaper.GetMasterTrack(0).then(t=>reaper.GetMediaTrackInfo_Value(t,"D_PAN"))')
    assert before!=after and ['pointerup',True] in ev('pointerLog'),(before,after,ev('pointerLog'))
    report.append('SVG knob changes actual master pan and releases outside its region')
    cmd('state');click(rect['x']+1,rect['y']+1)
    state=cmd('state');assert any(e['window']=='main' and e['type']==1 for e in state['events']),state
    report.append('SVG bounding-box corner reaches the native main window')
    ev('reaper.window.setMouseMode("passthrough")');cmd('state')
    click(400,400);mouse('wheel',400,400);mouse('down',400,400);mouse('drag',450,420);mouse('up',450,420)
    state=cmd('state')
    for kind in [1,2,6,22]:assert any(e['window']=='main' and e['type']==kind for e in state['events']),state
    ev('reaper.window.setMouseMode("regions")')
    report.append('Native clicks, wheel, drag and release reach REAPER in decoration mode')
    opacity_control(ev,mouse)
    report.append('Background opacity slider works in regions mode without fading controls or decoration')
    window_mode(ev,mouse,wait)
    rect=ev('(()=>{const r=document.querySelector("#pan").getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height}})()')
    x,y=rect['x']+rect['width']/2,rect['y']+rect['height']/2
    report.append('English/Chinese switching and native move/resize on a transparent background')
    raw("await reaper.InsertTrackAtIndex(0,false);window.track=await reaper.GetTrack(0,0);window.fx=await reaper.TrackFX_AddByName(track,'overlay/probe.jsfx',false,-1);await reaper.TrackFX_Show(track,fx,3);return fx")
    time.sleep(.5)
    for target in ['overlay','main','overlay']:
        cmd('focus',target=target);time.sleep(.3);state=cmd('state')
        floats=[i for i,w in enumerate(state['windows']) if 'Overlay native FX' in w['title']]
        assert floats and all(i<state['overlay']<state['main'] for i in floats),state
    click(x,y);state=cmd('state')
    assert all(i<state['overlay']<state['main'] for i,w in enumerate(state['windows']) if 'Overlay native FX' in w['title']),state
    report.append('Real floating JSFX stays above the interactive SVG overlay after overlay/main activation')
    description=ev('reaper.kbd_getTextFromCmd(40153,0)')
    assert 'MIDI' in description,description
    raw('await reaper.TrackFX_Show(track,fx,2);window.item=await reaper.CreateNewMIDIItemInProj(track,0,4,false);await reaper.SetMediaItemSelected(item,true);await reaper.Main_OnCommand(40153,0);return true')
    time.sleep(.5)
    for target in ['overlay','main']:
        cmd('focus',target=target);time.sleep(.3);state=cmd('state')
        floats=[i for i,w in enumerate(state['windows']) if 'MIDI' in w['title']]
        assert floats and all(i<state['overlay']<state['main'] for i in floats),state
    report.append('Real floating MIDI editor stays above the overlay')
    assert 'toolbar 1' in ev('reaper.kbd_getTextFromCmd(41679,0)').lower()
    ev('reaper.Main_OnCommand(41679,0)');time.sleep(.5)
    for target in ['overlay','main']:
        cmd('focus',target=target);time.sleep(.3);state=cmd('state')
        floats=[i for i,w in enumerate(state['windows']) if 'oolbar' in w['title']]
        assert floats and all(i<state['overlay']<state['main'] for i in floats),state
    report.append('Real floating toolbar stays above the overlay')
