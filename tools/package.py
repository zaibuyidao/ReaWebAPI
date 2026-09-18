"""Validate the install tree and emit build metadata and checksums."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('--stage', type=Path, required=True)
parser.add_argument('--platform', choices=['windows', 'macos', 'linux'], required=True)
parser.add_argument('--arch', required=True)
parser.add_argument('--revision', default='local')
parser.add_argument('--archive', type=Path)
args = parser.parse_args()
extension = {'windows': 'dll', 'macos': 'dylib', 'linux': 'so'}[args.platform]
filename = f'reaper_reawebapi-{args.arch}.{extension}'
binary = args.stage / 'UserPlugins' / filename
if not binary.is_file() or binary.stat().st_size == 0:
    raise SystemExit(f'Missing extension: {binary}')
required = ['Scripts/ReaWebAPI/Example/Example.lua', 'Scripts/ReaWebAPI/Example/index.html',
            'Scripts/ReaWebAPI/Example/app.js', 'Scripts/ReaWebAPI/Example/style.css',
            'ReaWebAPI/SDK/reaper.d.ts', 'ReaWebAPI/README.md', 'ReaWebAPI/THIRD_PARTY.md']
for relative in required:
    if not (args.stage / relative).is_file():
        raise SystemExit(f'Missing bundle file: {relative}')
metadata = {'version': '0.1.0', 'platform': args.platform, 'architecture': args.arch,
            'revision': args.revision, 'extension': filename}
(args.stage / 'ReaWebAPI' / 'BUILD.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
checksums = args.stage / 'SHA256SUMS.txt'
lines = []
for file in sorted(args.stage.rglob('*')):
    if file.is_file() and file != checksums:
        digest = hashlib.sha256(file.read_bytes()).hexdigest()
        lines.append(f'{digest}  {file.relative_to(args.stage).as_posix()}')
checksums.write_text('\n'.join(lines) + '\n', encoding='utf-8')
if args.archive:
    args.archive.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.archive, 'w', zipfile.ZIP_DEFLATED) as archive:
        for file in sorted(args.stage.rglob('*')):
            if file.is_file():
                archive.write(file, file.relative_to(args.stage).as_posix())
print(f'Validated {filename}; {len(lines)} files checksummed')
