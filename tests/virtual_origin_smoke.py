"""Isolated real-REAPER acceptance for virtual origins, identity and browser storage."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--reaper', type=Path, required=True)
parser.add_argument('--extension', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--ini', type=Path)
parser.add_argument('--allow-hidden', action='store_true', help='Record the animation-frame check as skipped if the host session hides every page')
args = parser.parse_args()
root = args.output.resolve()
root.mkdir(parents=True, exist_ok=False)
(root / 'UserPlugins').mkdir()
binary = root / 'UserPlugins' / args.extension.name
shutil.copy2(args.extension, binary)
if sys.platform == 'darwin':
    subprocess.run(['codesign', '--force', '--sign', '-', str(binary)], check=True)
if sys.platform.startswith('linux'):
    for helper in args.extension.parent.glob('reawebapi-webview-*'):
        shutil.copy2(helper, root / 'UserPlugins' / helper.name)
if args.ini:
    shutil.copy2(args.ini, root / 'reaper.ini')
else:
    (root / 'EmptyPlugins').mkdir()
    (root / 'reaper.ini').write_text('[REAPER]\naudiomode=4\nnewprojdo=0\nloadlastproj=0\nshowlastproj=0\nsplash=0\nverchk=0\nhasrecentlyopened=1\n'
        f'vstpath64={root / "EmptyPlugins"}\nvstfullstate=49989\n', encoding='utf-8')
source = Path(__file__).resolve().parents[1] / 'examples/runtime-features/web-runtime'
identities = {'A': 'timefold', 'B': 'reagba', 'Fallback': 'zaibuyidao-reagba'}
folders = {name: root / name for name in identities}
for name, folder in folders.items():
    shutil.copytree(source, folder)
    if name == 'Fallback':
        (folder / 'app.json').unlink()
    else:
        (folder / 'app.json').write_text(json.dumps({'id': identities[name], 'name': name, 'version': '1.0.0'}))
    page = folder / 'index.html'
    page.write_text(page.read_text(encoding='utf-8').replace('<title>', '<meta http-equiv="Content-Security-Policy" content="default-src \'self\'; script-src \'self\'; style-src \'self\'; connect-src \'self\'; worker-src \'self\' blob:"><title>'), encoding='utf-8')
duplicate = root / 'Duplicate'
shutil.copytree(folders['A'], duplicate)
launcher = root / 'zaibuyidao_ReaGBA.lua'
reports = []
phases = ('initial', 'restart', 'moved', 'entry-missing', 'rebound-restart')
for phase in phases:
    if phase == 'moved':
        record_path = root / 'ReaWebAPI/Apps' / identities['A'] / 'origin.json'
        legacy = json.loads(record_path.read_text())
        legacy.pop('entry')
        record_path.write_text(json.dumps(legacy), encoding='utf-8')
        destination = root / 'Renamed App 音 # %'
        folders['A'].rename(destination)
        folders['A'] = destination
    if phase == 'entry-missing':
        destination = root / 'Recovered Entry App'
        shutil.copytree(folders['A'], destination)
        (folders['A'] / 'index.html').unlink()
        (folders['A'] / 'other.html').write_text('<html>Not the original entry</html>')
        folders['A'] = destination
    expected = []
    for index, (name, folder) in enumerate(folders.items()):
        output = root / f'{phase}-{name}.json'
        expected.append(output)
        with (folder / 'app.js').open('w', encoding='utf-8') as stream:
            stream.write((source / 'app.js').read_text(encoding='utf-8').replace('window.runtimeCheck = run();',
                'window.runtimeCheck = reaper.lifecycle.ready.then(async () => {'
                ' await reaper.window.show(); await reaper.window.setSize(440, 500);'
                f' await reaper.window.setPosition({40 + index * 460}, 70);'
                ' await reaper.window.focus(); return run(); });'))
            stream.write('\nwindow.runtimeCheck.then(async report => {\n')
            stream.write("report.appId = await reaper.app.getId(); report.dataPath = await reaper.app.getDataPath();\n")
            stream.write("report.sourceLocation = new Error('source-location').stack;\n")
            stream.write('await reaper.fs.writeText(' + json.dumps(output.as_posix()) + ', JSON.stringify(report), {overwrite:true});\n});\n')
    paths = '{' + ','.join(json.dumps((folder / 'index.html').as_posix(), ensure_ascii=False) for folder in folders.values()) + '}'
    results = '{' + ','.join(json.dumps(path.as_posix()) for path in expected) + '}'
    launcher.write_text('''local ok, err = xpcall(function()
local ids = {}
local source = debug.getinfo(1, "S").source
for _, path in ipairs(PATHS) do
  local id = reaper.ReaWeb_Open(path, source, path)
  if id == 0 then
    local f = assert(io.open(ERROR, 'w')); f:write(reaper.ReaWeb_GetLastError()); f:close()
    reaper.defer(function() reaper.Main_OnCommand(40004, 0) end); return
  end
  ids[#ids + 1] = id
end
local duplicate = reaper.ReaWeb_Open(DUPLICATE, '@duplicate.lua')
local f = assert(io.open(CONFLICT, 'w'))
f:write(tostring(duplicate)..' '..reaper.ReaWeb_GetLastError()); f:close()
local started = reaper.time_precise()
local function tick()
  local count = 0
  for _, path in ipairs(RESULTS) do local f = io.open(path, 'r'); if f then f:close(); count = count + 1 end end
  if count == 3 or reaper.time_precise() - started > 50 then
    for _, id in ipairs(ids) do reaper.ReaWeb_Close(id) end
    reaper.defer(function() reaper.Main_OnCommand(40004, 0) end)
  else reaper.defer(tick) end
end
reaper.defer(tick)
end, debug.traceback)
if not ok then
  local f = assert(io.open(ERROR, 'w')); f:write(err); f:close()
  reaper.defer(function() reaper.Main_OnCommand(40004, 0) end)
end
'''.replace('PATHS', paths).replace('RESULTS', results)
        .replace('ERROR', json.dumps((root / f'error-{phase}.txt').as_posix()))
        .replace('DUPLICATE', json.dumps((duplicate / 'index.html').as_posix()))
        .replace('CONFLICT', json.dumps((root / f'conflict-{phase}.txt').as_posix())), encoding='utf-8')
    with (root / f'process-{phase}.log').open('w') as log:
        process = subprocess.Popen([str(args.reaper.resolve()), '-newinst', '-cfgfile', str(root / 'reaper.ini'),
            '-new', '-ignoreerrors', '-nosplash', str(launcher)], stdout=log, stderr=log)
        try:
            process.wait(timeout=70)
        except subprocess.TimeoutExpired:
            process.terminate(); process.wait(timeout=10); raise
    error = root / f'error-{phase}.txt'
    assert not error.exists(), error.read_text(encoding='utf-8')
    conflict = (root / f'conflict-{phase}.txt').read_text()
    assert conflict.startswith('0 ') and 'APP_ID_CONFLICT' in conflict, conflict
    for name, result in zip(identities, expected):
        report = json.loads(result.read_text(encoding='utf-8'))
        skipped = ['Timers / requestAnimationFrame'] if args.allow_hidden and report.get('visibility') == 'hidden' else []
        assert all(check['ok'] for check in report['checks'] if check['name'] not in skipped and not check['name'].startswith(('Cookies', 'WebGL'))), report
        report['skippedChecks'] = skipped
        assert report['origin'] == 'reaweb://' + identities[name], report
        assert report['appId'] == identities[name], report
        assert report['runtime']['mode'] == 'app-virtual' and report['runtime']['localResources'], report
        visits = phases.index(phase) + 1
        assert report['storageVisits'] == report['indexedDBVisits'] == visits, report
        assert report['origin'] + '/app.js' in report['sourceLocation'], report
        record = json.loads((root / 'ReaWebAPI/Apps' / identities[name] / 'origin.json').read_text())
        assert record['schema'] == 2 and record['entry'] == 'index.html' and 'port' not in record, record
        reports.append({'phase': phase, 'app': name, **report})
    print(phase + ': origins, fallback, conflict, storage, modules, Workers, CSP and source URLs passed'
        + (' (animation frames skipped in hidden host session)' if any(row['skippedChecks'] for row in reports[-3:]) else ''), flush=True)
(root / 'report.json').write_text(json.dumps(reports, ensure_ascii=False, indent=2), encoding='utf-8')
