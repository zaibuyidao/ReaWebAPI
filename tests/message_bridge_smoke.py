"""Run real Lua/WebView bridge and example checks in a disposable REAPER instance."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reaper', type=Path, required=True)
parser.add_argument('--extension', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True, help='New isolated resource directory')
args = parser.parse_args()
root = args.output.resolve()
root.mkdir(parents=True, exist_ok=False)
(root / 'UserPlugins').mkdir()
(root / 'reaper.ini').write_text('[REAPER]\n', encoding='utf-8')
binary = root / 'UserPlugins' / args.extension.name
shutil.copy2(args.extension, binary)
if sys.platform == 'darwin':
    subprocess.run(['codesign', '--force', '--sign', '-', str(binary)], check=True)
elif sys.platform.startswith('linux'):
    for helper in args.extension.parent.glob('reawebapi-webview-*'):
        shutil.copy2(helper, root / 'UserPlugins' / helper.name)
source = Path(__file__).resolve().parents[1]
script = root / 'message_bridge_smoke.lua'
shutil.copy2(source / 'tests/message_bridge_smoke.lua', script)
demo = root / 'example'
shutil.copytree(source / 'web/lua-backend', demo)
with (demo / 'index.html').open('a', encoding='utf-8') as page:
    page.write('''<script>
const probe = setInterval(() => {
  if (document.querySelector('#status').textContent !== 'Connected to Lua') return;
  clearInterval(probe);
  const volume = document.querySelector('#volume');
  if (document.querySelector('#name').textContent !== '鼓组 "A"\\n\\t🎛') {
    reaper.host.send('example failed: track name'); return;
  }
  volume.value = '0.5';
  volume.dispatchEvent(new Event('change'));
}, 50);
</script>''')
report = root / 'result.txt'
command = [str(args.reaper.resolve()), '-newinst', '-cfgfile', str(root / 'reaper.ini'), '-new', '-nosplash', str(script)]
with (root / 'process.log').open('w', encoding='utf-8') as log:
    process = subprocess.Popen(command, stdout=log, stderr=log)
    try:
        deadline = time.monotonic() + 60
        while not report.exists() and process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.2)
        result = report.read_text(encoding='utf-8') if report.exists() else 'FAIL: no result'
        print(json.dumps({'resource': str(root), 'result': result}, ensure_ascii=False), flush=True)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
if not result.startswith('PASS:'):
    raise SystemExit(1)
