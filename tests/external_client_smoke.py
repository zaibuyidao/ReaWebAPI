"""Opt-in External Client check against an isolated real REAPER process."""
import argparse
import json
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import time

from external_client_contract import WebSocket, check_external_methods, project_snapshot, subscribe_projects


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reaper', type=Path, required=True)
    parser.add_argument('--extension', type=Path, required=True)
    parser.add_argument('--stream-extension', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--startup-timeout', type=float, default=45)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    (root / 'UserPlugins').mkdir()
    for source in (args.extension, args.stream_extension):
        target = root / 'UserPlugins' / source.name
        shutil.copy2(source, target)
        if sys.platform == 'darwin':
            subprocess.run(['codesign', '--force', '--sign', '-', str(target)], check=True)
    if sys.platform.startswith('linux'):
        for helper in args.extension.parent.glob('reawebapi-webview-*'):
            shutil.copy2(helper, root / 'UserPlugins' / helper.name)
    empty_vst = root / 'EmptyVST'
    empty_vst.mkdir()
    config = ('[REAPER]\nerrnowarn=5\nloadlastproj=0\nshowlastproj=0\nsplash=0\nverchk=0\n'
              f'vst_scan=0\nvstpath={empty_vst}\nvstpath64={empty_vst}\n')
    if sys.platform == 'win32':
        # Keep first-run VST3 path migration from adding system plug-in directories.
        config += 'vstfullstate=49989\n[audioconfig]\nmode=4\nwaveout_devicein=-1\nwaveout_deviceout=0\nwaveout_srate=48000\nwaveout_bps=16\nwaveout_bs=256\nwaveout_numblocks=4\nwaveout_nch_in=0\nwaveout_nch_out=2\n'
    if sys.platform == 'darwin':
        config += 'hasrecentlyopened=1\n[audioconfig]\nmode=4\n'
    if sys.platform.startswith('linux'):
        config += 'linux_audio_mode=3\nlinux_audio_srate=48000\nlinux_audio_bsize=512\n'
    (root / 'reaper.ini').write_text(config)
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    token = secrets.token_hex(32)
    (root / 'ReaWebAPI.ini').write_text(f'[ExternalClients]\nEnabled=1\nPort={port}\nAccessToken={token}\n')
    (root / 'index.html').write_text('<!doctype html><title>External coexistence test</title><script src="app.js" defer></script>')
    fixture = Path(__file__).resolve().parent / 'fixtures/retired-mirror-names.json'
    shutil.copyfile(fixture, root / 'former-methods.json')
    (root / 'app.js').write_text("""(async () => {
      const caps = await reaper.lifecycle.ready;
      const names = await (await fetch('former-methods.json')).json();
      if (names.length !== 730 || new Set(names).size !== 730 || names.some(n => n in reaper || caps.methods.includes(n)) ||
          'batch' in reaper.transaction || 'setBufferSize' in reaper.debug ||
          'api' in caps || 'batchMethods' in caps) throw Error('Retired WebView API exposed');
      // Exercise the actual wire dispatcher with a valid document and request ID.
      for (const [method,args] of [['CountTracks',[0]],['ReaWeb_Batch',[[{method:'CountTracks',args:[0]}]]],
                                   ['ReaWeb_SetBufferSize',[65536]]]) {
        const stringify = JSON.stringify;
        JSON.stringify = function(value, ...rest) {
          if (value?.method === 'ReaWeb_GetWindowState') value = {...value,method,args};
          return stringify(value, ...rest);
        };
        try {
          await reaper.window.getState();
          throw Error('Unexpected native success: ' + method);
        } catch (error) { if (error.code !== 'UNKNOWN_API') throw error; }
        finally { JSON.stringify = stringify; }
      }
      await reaper.window.getState();
      const count = async () => (await reaper.host.service('runtime').invoke('getProjectSummary')).trackCount;
      await reaper.events.on('trackStateChanged', async () => {
        await reaper.fs.writeText('event.json', JSON.stringify({count:await count()}), {overwrite:true});
      });
      await reaper.fs.writeText('web-ready.json', JSON.stringify({count:await count()}), {overwrite:true});
    })().catch(error => reaper.fs.writeText('web-error.txt', String(error), {overwrite:true}));""")
    (root / 'launch.lua').write_text('''local root = debug.getinfo(1, 'S').source:sub(2):match('^(.*[/\\\\])')
local file = assert(io.open(root .. 'launcher.txt', 'w'))
file:write(reaper.GetResourcePath(), '\\n', tostring(reaper.APIExists('ReaWeb_Open')))
file:close()
assert(reaper.ReaWeb_Open(root .. 'index.html') > 0, reaper.ReaWeb_GetLastError())
local function requested(name)
  local path = root .. name
  local f = io.open(path, 'r')
  if not f then return false end
  f:close()
  assert(os.remove(path))
  return true
end
local function tick()
  if requested('add-track') then
    reaper.InsertTrackAtIndex(reaper.CountTracks(0), true)
    reaper.TrackList_AdjustWindows(false)
    reaper.UpdateArrange()
  elseif requested('reload-project') then
    local fixture = root .. 'fixture.rpp'
    reaper.Main_SaveProjectEx(0, fixture, 0)
    reaper.Main_openProject('noprompt:' .. fixture)
  end
  reaper.defer(tick)
end
tick()
''')
    clients = []
    with (root / 'process.log').open('w') as log:
        startup = None
        if sys.platform == 'win32':
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
        process = subprocess.Popen([str(args.reaper.resolve()), '-newinst', '-cfgfile', str(root / 'reaper.ini'),
                                    '-new', '-nosplash', '-splashlog', str(root / 'startup.log'), str(root / 'launch.lua')],
                                   stdout=log, stderr=log, startupinfo=startup)
        (root / 'process.pid').write_text(str(process.pid))
        try:
            deadline = time.monotonic() + args.startup_timeout
            while True:
                try:
                    a = WebSocket(f'ws://127.0.0.1:{port}')
                    clients.append(a)
                    break
                except OSError as error:
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(f'REAPER did not accept connections, exit code: {process.poll()}. See {root / "startup.log"} and {root / "process.log"}') from error
                    time.sleep(0.1)
            a.call('system.getInfo', error='AUTH_REQUIRED')
            a.call('auth.authenticate', {'token': token, 'protocolVersion': 1})
            check_external_methods(a)
            info = a.call('system.getInfo')
            assert a.call('service.invoke', {'service': 'runtime', 'method': 'getInfo'})['version'] == info['version']
            ready = root / 'web-ready.json'
            while not ready.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            assert ready.exists(), (root / 'web-error.txt').read_text() if (root / 'web-error.txt').exists() else 'WebView failed to start'
            initial = json.loads(ready.read_text())['count']
            summary = a.call('service.invoke', {'service': 'runtime', 'method': 'getProjectSummary'})
            assert summary['trackCount'] == initial and isinstance(summary['playState'], int)
            assert a.call('service.invoke', {'service': 'runtime', 'method': 'getHostVersion'}) == info['reaperVersion']
            b = WebSocket(f'ws://127.0.0.1:{port}'); clients.append(b)
            b.call('auth.authenticate', {'token': token, 'protocolVersion': 1})
            sub_a, sub_b = subscribe_projects(a), subscribe_projects(b)
            old_a, old_b = project_snapshot(a, sub_a), project_snapshot(b, sub_b)
            assert old_a['activeProject'] != old_b['activeProject']
            event = a.call('events.subscribe', {'event': 'track-state-changed'})['subscriptionId']
            (root / 'add-track').touch()
            legacy = root / 'event.json'
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                a.call('system.getInfo')
                try:
                    if json.loads(legacy.read_text())['count'] == initial + 1 and any(e.get('subscriptionId') == event for e in a.events):
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(0.05)
            assert json.loads(legacy.read_text())['count'] == initial + 1, (a.events, a.call('service.invoke', {'service': 'runtime', 'method': 'getProjectSummary'}))
            assert any(e.get('subscriptionId') == event for e in a.events)
            (root / 'reload-project').touch()
            new_a = project_snapshot(a, sub_a, old_a['projectEpoch'])
            new_b = project_snapshot(b, sub_b, old_b['projectEpoch'])
            assert len({p['activeProject'] for p in (old_a, old_b, new_a, new_b)}) == 4
            assert a.call('events.unsubscribe', {'subscriptionId': sub_a}) is True
            a.call('service.invoke', {'service': 'stream.test', 'method': 'start'})
            stream = a.call('stream.open', {'name': 'test.video'})
            binary = WebSocket(stream['endpoint']); clients.append(binary)
            opcode, packet = binary.receive_frame()
            assert opcode == 2 and packet[:4] == b'RWS\x01' and len(packet) == 40 + 240 * 160 * 4
            binary.frame(b'\1', opcode=2)
            assert a.call('stream.close', {'consumerId': stream['consumerId']}) is True
            try:
                while binary.receive_frame()[0] != 8:
                    pass
            except (EOFError, ConnectionResetError):
                pass
            binary.close()
            a.close()
            assert b.call('service.invoke', {'service': 'runtime', 'method': 'getProjectSummary'})['trackCount'] == initial + 1
            c = WebSocket(f'ws://127.0.0.1:{port}'); clients.append(c)
            c.call('auth.authenticate', {'token': token, 'protocolVersion': 1})
            fresh = project_snapshot(c, subscribe_projects(c))
            assert fresh['activeProject'] not in {new_a['activeProject'], new_b['activeProject']}
            b.call('service.invoke', {'service': 'stream.test', 'method': 'stop'})
            report = {'passed': True, 'info': info, 'checks': ['authentication', 'external mirror removal and wire rejection',
                'WebView mirror removal and wire rejection', 'named service', 'track events', 'project reload',
                'session-scoped project IDs', 'unsubscribe', 'binary stream open/read/close', 'reconnect']}
            (root / 'result.json').write_text(json.dumps(report, indent=2))
            print(json.dumps(report), flush=True)
        finally:
            for client in clients:
                client.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()


if __name__ == '__main__':
    main()
