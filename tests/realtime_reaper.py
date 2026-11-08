"""Measure realtime transport against frame events in a disposable REAPER profile."""
import argparse
import configparser
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
for name in ('reaper', 'extension', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
parser.add_argument('--duration', type=float, default=10)
parser.add_argument('--audio-config', type=Path, help='Use an existing Windows audioconfig section in the disposable profile')
parser.add_argument('--block-size', type=int, help='Request an audio buffer size in the disposable profile')
args = parser.parse_args()
root = args.output.resolve()
root.mkdir(parents=True, exist_ok=False)
source = Path(__file__).resolve().parents[1]
for path in (source / 'examples/integrations/native-transport').iterdir():
    if path.is_file():
        shutil.copy2(path, root / path.name)
(root / 'UserPlugins').mkdir()
binary = root / 'UserPlugins' / args.extension.name
shutil.copy2(args.extension, binary)
if sys.platform == 'darwin':
    subprocess.run(['codesign', '--force', '--sign', '-', str(binary)], check=True)
if sys.platform.startswith('linux'):
    helper = next(args.extension.parent.glob('reawebapi-webview-*'))
    shutil.copy2(helper, root / 'UserPlugins' / helper.name)
    (root / 'UserPlugins' / helper.name).chmod(0o755)
(root / 'EmptyVST').mkdir()
settings = ('[reaper]\nloadlastproj=0\nshowlastproj=0\nsplash=0\nverchk=0\nerrnowarn=5\nhasrecentlyopened=1\n'
            'newprojdo=0\nvst_scan=0\nvstfullstate=49989\nvstpath64=' + str(root / 'EmptyVST') + '\n')
if sys.platform == 'darwin':
    user = configparser.RawConfigParser(strict=False)
    user.read(Path.home() / 'Library/Application Support/REAPER/reaper.ini')
    if user.has_section('reaper'):
        settings += ''.join(f'{key}={value}\n' for key, value in user['reaper'].items() if key.startswith('coreaudio'))
elif sys.platform.startswith('linux'):
    settings += 'linux_audio_mode=3\nlinux_audio_srate=48000\nlinux_audio_bsize=256\nlinux_audio_nch_out=2\n'
else:
    settings += '[audioconfig]\nmode=4\nwaveout_devicein=-1\nwaveout_deviceout=0\nwaveout_srate=48000\nwaveout_bps=16\nwaveout_bs=256\nwaveout_numblocks=4\nwaveout_nch_out=2\n'
(root / 'reaper.ini').write_text(settings, encoding='utf-8')
if args.audio_config:
    audio = configparser.RawConfigParser(strict=False)
    audio.read(args.audio_config, encoding='utf-8-sig')
    if not audio.has_section('audioconfig'):
        raise ValueError('Missing audioconfig section')
    settings = settings.split('[audioconfig]')[0] + '[audioconfig]\n' + ''.join(f'{key}={value}\n' for key, value in audio['audioconfig'].items())
    (root / 'reaper.ini').write_text(settings, encoding='utf-8')
if args.block_size:
    config = configparser.RawConfigParser(strict=False)
    config.read(root / 'reaper.ini', encoding='utf-8')
    if sys.platform == 'darwin':
        config['reaper']['coreaudiobs'] = str(args.block_size)
        config['reaper']['coreaudiobsuse'] = '1'
    elif sys.platform == 'win32':
        config['audioconfig']['asio_bsize'] = str(args.block_size)
        config['audioconfig']['asio_bsize_use'] = '1'
    else:
        config['reaper']['linux_audio_bsize'] = str(args.block_size)
    with (root / 'reaper.ini').open('w', encoding='utf-8') as file:
        config.write(file)
resource = Path.home() / 'Library/Application Support/REAPER' if sys.platform == 'darwin' else args.reaper.parent
for name in ('reaper-install-rev.txt', 'reaper-license.rk'):
    if (resource / name).is_file():
        shutil.copy2(resource / name, root / name)
probe = r'''
(async () => {
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  const results = [], checks = {}, audioDevice = {};
  try {
    await window.demoReady;
    audioDevice.sampleRate = await reaper.GetAudioDeviceInfo('SRATE');
    audioDevice.blockSize = await reaper.GetAudioDeviceInfo('BSIZE');
    await reaper.window.setSize(1050, 1000);
    await wait(1000);
    const initial = await reaper.realtime.getDiagnostics();
    checks.transportPayloadClean = true;
    const diagnosticShape = d => Object.keys(d).sort().join(',') === 'schedulerRate,skippedBusy,sources,subscriptions' && Object.keys(d.sources.transport).sort().join(',') === 'errors,lastSampleAt,maxSampleIntervalMs,sampled';
    checks.basicDiagnosticsOnly = diagnosticShape(initial);
    checks.englishDefault = document.documentElement.lang === 'en' && el('play').textContent === 'Play';
    const beforeSwitch = {id:initial.subscriptions[0].id, total};
    el('language').value = 'zh-CN'; el('language').onchange();
    checks.chineseSwitch = document.documentElement.lang === 'zh-CN' && el('play').textContent === '播放' && el('pause').textContent === '暂停 / 继续';
    el('language').value = 'en'; el('language').onchange();
    checks.englishSwitch = document.documentElement.lang === 'en' && el('pause').textContent === 'Pause / Resume' && total === beforeSwitch.total && (await reaper.realtime.getDiagnostics()).subscriptions[0].id === beforeSwitch.id;
    checks.csvTransportOnly = csv(samples).split('\n')[0] === 'received_page_ms,arrival_interval_ms,sampled_host_seconds,sample_interval_ms,sequence,position_seconds,play_state,sequence_gap,project_epoch,play_rate,tempo_bpm,qn_position,time_signature_numerator,time_signature_denominator';
    await reaper.SetTempoTimeSigMarker(0, -1, 0, -1, -1, 90, 4, 4, true);
    await reaper.SetTempoTimeSigMarker(0, -1, 2, -1, -1, 150, 7, 8, false);
    await reaper.SetTempoTimeSigMarker(0, -1, 4, -1, -1, 110, 3, 4, false);
    const musical = await reaper.realtime.subscribe('transport', {updateRate:120});
    const musicResults = [];
    async function checkMusic(phase) {
      await wait(100);
      const v = musical.latest();
      const sig = await reaper.TimeMap_GetTimeSigAtTime(0, v.position);
      const qn = await reaper.TimeMap_timeToQN_abs(0, v.position);
      const rate = await reaper.Master_GetPlayRateAtTime(v.position, 0);
      const passed = Math.abs(v.tempo - sig[2]) < 1e-8 && v.timeSignature.numerator === sig[0] &&
        v.timeSignature.denominator === sig[1] && Math.abs(v.beatPosition - qn) < 1e-8 && Math.abs(v.playRate-rate) < 1e-8;
      musicResults.push({phase, position:v.position, tempo:v.tempo, sig, qn, rate, passed});
      if (!passed) throw new Error('Musical snapshot mismatch: '+JSON.stringify(musicResults.at(-1)));
    }
    await reaper.OnPlayButton();
    for (const pos of [0.5, 1.25, 2, 2.75, 4.25]) {
      await reaper.SetEditCurPos(pos, false, true); await checkMusic('seek-'+pos);
    }
    await reaper.CSurf_OnPlayRateChange(1.25); await checkMusic('rate-change');
    await reaper.OnPauseButton(); await checkMusic('pause');
    await reaper.OnStopButton(); await checkMusic('stop');
    await reaper.GetSet_LoopTimeRange(true, true, 1, 3, false);
    await reaper.GetSetRepeat(1); await reaper.SetEditCurPos(1, false, true); await reaper.OnPlayButton();
    let wrapped = false, last = -1;
    for (let i=0;i<35;i++) { await checkMusic('loop'); const pos=musical.latest().position; if(pos<last)wrapped=true; last=pos; }
    if (!wrapped) throw new Error('Loop did not wrap');
    await reaper.OnStopButton(); await reaper.GetSetRepeat(0); await reaper.CSurf_OnPlayRateChange(1);
    await musical.close(); checks.musical = musicResults; checks.loopWrapped = wrapped;
    let frames = 0;
    const frameOff = await reaper.events.on('frame', () => frames++);
    for (const [name, selected, command, duration] of [
      ['frame-playing', 'frame', () => reaper.OnPlayButton(), DURATION],
      ['60-playing', '60', () => reaper.OnPlayButton(), DURATION],
      ['120-playing', '120', () => reaper.OnPlayButton(), DURATION],
      ['120-paused', '120', () => reaper.OnPauseButton(), 3000],
      ['120-stopped', '120', () => reaper.OnStopButton(), 3000],
      ['120-device-closed', '120', () => reaper.Audio_Quit(), 3000]]) {
      if(mode!==selected)await switchMode(selected); await command(); await wait(500); reset(); await wait(duration);
      const rows = samples.slice(), arrival = metrics(rows,'receivedMs');
      const native = metrics(rows.filter(r=>r.sampledAt!==null).map(r=>({...r,t:r.sampledAt*1000})),'t');
      const span = rows.length>1 ? (rows.at(-1).receivedMs-rows[0].receivedMs)/1000 : 0;
      const changes = rows.slice(1).filter((r,i)=>r.position!==rows[i].position).length;
      checks.transportPayloadClean = checks.transportPayloadClean && latest !== null && (selected === 'frame' || Object.keys(latest).sort().join(',') === ['source','position','playState','playRate','tempo','timeSignature','beatPosition','sampledAt','sampleDurationMs','sequence','sourceSequence','projectEpoch','projectChanged','producerDropped'].sort().join(','));
      results.push({phase:name,frames:rows.length,arrival,native,positionChangesHz:span?changes/span:0,
        states:[...new Set(rows.map(r=>r.state))],gaps,firstPosition:rows[0]?.position,lastPosition:rows.at(-1)?.position,
        diagnostics:await reaper.realtime.getDiagnostics()});
      await reaper.fs.writeText(name+'.csv',csv(rows),{overwrite:true});
    }
    await reaper.Audio_Init();
    const before = frames; await wait(250); checks.frameContinues = frames>before;
    checks.luaWrites = (await reaper.debug.getDiagnostics()).frames.luaWrites;
    const first = await reaper.realtime.subscribe('transport',{updateRate:60});
    const second = await reaper.realtime.subscribe('transport',{updateRate:120});
    await first.close(); await wait(500);
    checks.secondContinues = second.latest() !== null && !second.closed;
    await second.close();
    if(dispose){await dispose();dispose=null;}
    await frameOff();
    checks.cleaned = (await reaper.realtime.getDiagnostics()).subscriptions.length===0;
    const idle = await reaper.realtime.getDiagnostics();
    checks.basicDiagnosticsOnly = checks.basicDiagnosticsOnly && diagnosticShape(idle) && idle.schedulerRate === 0 && results.every(r=>diagnosticShape(r.diagnostics));
    const rates = results.filter(r=>['60-playing','120-playing'].includes(r.phase));
    const passed = rates.every(r=>r.native.hz>=Number(r.phase.split('-')[0])*.9 && r.arrival.hz>=Number(r.phase.split('-')[0])*.85 && r.lastPosition>r.firstPosition) &&
      results.find(r=>r.phase==='120-paused').states.every(s=>s===2) &&
      results.filter(r=>r.phase==='120-stopped'||r.phase==='120-device-closed').every(r=>r.frames>100&&r.states.every(s=>s===0)) &&
      checks.frameContinues && checks.luaWrites===0 && checks.secondContinues && checks.cleaned &&
      checks.transportPayloadClean && checks.basicDiagnosticsOnly && checks.englishDefault && checks.chineseSwitch && checks.englishSwitch && checks.csvTransportOnly;
    await reaper.fs.writeText('result.json',JSON.stringify({passed,scope:'native-sampling-and-web-delivery',version:await reaper.GetAppVersion(),audioDevice,results,checks},null,2),{overwrite:true});
  } catch(error) {
    await reaper.fs.writeText('result.json',JSON.stringify({passed:false,error:String(error),stack:error.stack,results,checks},null,2),{overwrite:true});
  }
})();
'''.replace('DURATION', str(round(args.duration * 1000)))
(root / 'probe.js').write_text(probe, encoding='utf-8')
with (root / 'index.html').open('a', encoding='utf-8') as file:
    file.write('<script src="probe.js" defer></script>')
with (root / 'process.log').open('w') as log:
    startup = None
    if sys.platform == 'win32':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    process = subprocess.Popen([str(args.reaper.resolve()), '-newinst', '-cfgfile', str(root / 'reaper.ini'),
                                '-new', '-ignoreerrors', '-nosplash', str(root / 'reawebapi-native-transport.lua')],
                               stdout=log, stderr=log, startupinfo=startup)
    try:
        deadline = time.monotonic() + args.duration * 3 + 100
        while time.monotonic() < deadline:
            if (root / 'result.json').exists():
                result = json.loads((root / 'result.json').read_text())
                print(json.dumps(result, indent=2), flush=True)
                if not result['passed']:
                    raise AssertionError('Realtime acceptance failed')
                break
            if process.poll() is not None:
                raise RuntimeError(f'REAPER exited with {process.returncode}')
            time.sleep(.2)
        else:
            raise TimeoutError('Realtime acceptance timed out. See disposable profile and process.log')
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
