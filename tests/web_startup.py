"""Opt-in WebView2 startup samples in the mock host's isolated profile."""
import ctypes as C
from ctypes import wintypes as W
import json
import time


def run_windows(host):
    folder = host['root'] / 'Scripts/startup'
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'app.json').write_text('{"id":"startup-probe"}', encoding='utf-8')
    page = folder / 'index.html'
    page.write_text('''<!doctype html><meta charset="utf-8"><style>html{background:#182838;color:white}</style>
<p id="name">Loading</p><script>
(async()=>{
  const start=performance.now();
  await reaper.lifecycle.ready;
  const readyMs=performance.now()-start;
  const track=await reaper.audio.getSelectedTrackRef();
  document.getElementById('name').textContent=(await reaper.audio.getTrackMix(track)).name;
  const usableMs=performance.now()-start;
  if(document.readyState!=='complete')await new Promise(r=>addEventListener('load',r,{once:true}));
  await reaper.host.send(JSON.stringify({readyMs,usableMs,navigation:performance.getEntriesByType('navigation')[0]?.toJSON(),
    resources:performance.getEntriesByType('resource').slice(0,64).map(({initiatorType,startTime,duration})=>({initiatorType,startTime,duration}))}));
})().catch(e=>reaper.host.send(JSON.stringify({error:e.message})));
</script>''', encoding='utf-8')
    user32 = C.windll.user32
    user32.PeekMessageW.argtypes = [C.POINTER(W.MSG), W.HWND, W.UINT, W.UINT, W.UINT]
    user32.TranslateMessage.argtypes = [C.POINTER(W.MSG)]
    user32.DispatchMessageW.argtypes = [C.POINTER(W.MSG)]
    diagnostics = C.CFUNCTYPE(C.c_char_p, C.c_int)(host['registrations'][b'API_ReaWeb_GetDiagnostics'])
    next_tick = 0
    tick_costs = []

    def pump():
        nonlocal next_tick
        message = W.MSG()
        for _ in range(100):
            if not user32.PeekMessageW(C.byref(message), None, 0, 0, 1):
                break
            user32.TranslateMessage(C.byref(message))
            user32.DispatchMessageW(C.byref(message))
        now = time.monotonic()
        if now >= next_tick:
            tick_started = time.perf_counter()
            host['timer']()
            tick_costs.append((time.perf_counter() - tick_started) * 1000)
            next_tick = now + .03
        time.sleep(.001)

    idle = time.monotonic()
    while time.monotonic() - idle < 1.5:
        pump()
    class Memory(C.Structure):
        _fields_ = [('cb', W.DWORD), ('PageFaultCount', W.DWORD)] + [(name, C.c_size_t) for name in (
            'PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage',
            'QuotaPeakNonPagedPoolUsage', 'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage')]
    memory = Memory()
    memory.cb = C.sizeof(memory)
    kernel = C.windll.kernel32
    kernel.GetCurrentProcess.restype = W.HANDLE
    psapi = C.windll.psapi
    psapi.GetProcessMemoryInfo.argtypes = [W.HANDLE, C.POINTER(Memory), W.DWORD]
    assert psapi.GetProcessMemoryInfo(kernel.GetCurrentProcess(), C.byref(memory), memory.cb)
    class ProcessEntry(C.Structure):
        _fields_ = [('size', W.DWORD), ('usage', W.DWORD), ('pid', W.DWORD), ('heap', C.c_size_t),
                    ('module', W.DWORD), ('threads', W.DWORD), ('parent', W.DWORD), ('priority', W.LONG),
                    ('flags', W.DWORD), ('name', W.WCHAR * 260)]
    kernel.CreateToolhelp32Snapshot.argtypes = [W.DWORD, W.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = W.HANDLE
    kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [W.HANDLE, C.POINTER(ProcessEntry)]
    kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    kernel.OpenProcess.restype = W.HANDLE
    kernel.CloseHandle.argtypes = [W.HANDLE]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    assert snapshot != C.c_void_p(-1).value
    processes = {}
    entry = ProcessEntry(); entry.size = C.sizeof(entry)
    try:
        found = kernel.Process32FirstW(snapshot, C.byref(entry))
        while found:
            processes[entry.pid] = entry.parent
            found = kernel.Process32NextW(snapshot, C.byref(entry))
    finally: kernel.CloseHandle(snapshot)
    owned = {kernel.GetCurrentProcessId()}
    while True:
        children = {pid for pid, parent in processes.items() if parent in owned} - owned
        if not children: break
        owned.update(children)
    private_bytes, process_count = 0, 0
    for pid in owned:
        handle = kernel.OpenProcess(0x410, False, pid)
        if not handle: continue
        try:
            sample = Memory(); sample.cb = C.sizeof(sample)
            if psapi.GetProcessMemoryInfo(handle, C.byref(sample), sample.cb):
                private_bytes += sample.PrivateUsage; process_count += 1
        finally: kernel.CloseHandle(handle)
    records = [{'kind': 'idle', 'firstTickMs': host['first_tick_ms'], 'tickMaxMs': max(tick_costs),
                'tickTotalMs': sum(tick_costs), 'hostPrivateBytes': memory.PrivateUsage, 'hostWorkingSetBytes': memory.WorkingSetSize,
                'processTreePrivateBytes': private_bytes, 'processCount': process_count}]
    opened = []
    for kind in ('first', 'new', 'reopen'):
        if kind == 'reopen':
            for identifier in opened:
                host['close_window'](identifier)
            deadline = time.monotonic() + 5
            while any(json.loads(diagnostics(identifier))['stage'] != 'closed' for identifier in opened):
                assert time.monotonic() < deadline
                pump()
            opened.clear()
        started = time.monotonic()
        identifier = host['open_window'](str(page).encode(), kind.encode())
        assert identifier, host['get_error']()
        opened.append(identifier)
        frontend = None
        while True:
            assert time.monotonic() - started < 20, json.loads(diagnostics(identifier))
            pump()
            message = host['host_receive'](identifier)
            if message:
                frontend = json.loads(message)
                assert 'error' not in frontend, frontend
            native = json.loads(diagnostics(identifier))
            if frontend and native['startup']['milestones'].get('revealCompleteMs') is not None:
                break
        records.append({'kind': kind, 'native': native['startup'], 'environment': native.get('environmentStartup'),
                        'controllerPrewarmed': native.get('controllerPrewarmed'), 'idleWebViews': native.get('idleWebViews'), 'frontend': frontend})
        reused = host['open_window'](str(page).encode(), kind.encode())
        assert reused == identifier
    for identifier in opened:
        host['close_window'](identifier)
    for _ in range(50):
        pump()
    (host['root'] / 'startup.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    print(json.dumps(records))
