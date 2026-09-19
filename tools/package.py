"""Validate the install tree and emit build metadata and checksums."""
import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path
import zipfile

if not __package__:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.package_sdk import stage_sdk
from tools.version import source_version

parser = argparse.ArgumentParser()
parser.add_argument('--stage', type=Path, required=True)
parser.add_argument('--platform', choices=['windows', 'macos', 'linux'], required=True)
parser.add_argument('--arch', required=True)
parser.add_argument('--revision', default='local')
parser.add_argument('--archive', type=Path)
parser.add_argument('--release-dir', type=Path)
args = parser.parse_args()
version = source_version()
extension = {'windows': 'dll', 'macos': 'dylib', 'linux': 'so'}[args.platform]
filename = f'reaper_reawebapi-{args.arch}.{extension}'
binary = args.stage / 'UserPlugins' / filename
if not binary.is_file() or binary.stat().st_size == 0:
    raise SystemExit(f'Missing extension: {binary}')
required = ['Scripts/ReaWebAPI/Example/Example.lua', 'Scripts/ReaWebAPI/Example/index.html',
            'Scripts/ReaWebAPI/Example/app.js', 'Scripts/ReaWebAPI/Example/style.css', 'Scripts/ReaWebAPI/Example/logo.svg',
            'ReaWebAPI/SDK/reaper.d.ts', 'ReaWebAPI/SDK/reaper-api.generated.d.ts',
            'ReaWebAPI/SDK/reaper-api.generated.js', 'ReaWebAPI/README.md', 'ReaWebAPI/README.zh-CN.md', 'ReaWebAPI/THIRD_PARTY.md']
native_files = [binary]
if args.platform == 'linux':
    helper = args.stage / 'UserPlugins' / f'reawebapi-webview-{args.arch}'
    if not helper.is_file() or not helper.stat().st_size:
        raise SystemExit(f'Missing WebKit helper: {helper}')
    native_files.append(helper)
for relative in required:
    if not (args.stage / relative).is_file():
        raise SystemExit(f'Missing bundle file: {relative}')
stage_sdk(args.stage, version, args.revision)
metadata = {'version': version, 'platform': args.platform, 'architecture': args.arch,
            'revision': args.revision, 'extension': filename}
(args.stage / 'ReaWebAPI' / 'BUILD.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
checksums = args.stage / 'SHA256SUMS.txt'
lines = []
for file in sorted(args.stage.rglob('*')):
    if file.is_file() and file != checksums:
        digest = hashlib.sha256(file.read_bytes()).hexdigest()
        lines.append(f'{digest}  {file.relative_to(args.stage).as_posix()}')
checksums.write_text('\n'.join(lines) + '\n', encoding='utf-8')
archive_path = args.archive
if args.release_dir:
    args.release_dir.mkdir(parents=True, exist_ok=True)
    for file in native_files:
        shutil.copy2(file, args.release_dir / file.name)
    archive_path = args.release_dir / f'ReaWebAPI-{args.platform}-{args.arch}-v{version}.zip'
if archive_path:
    archive_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive_path, 'w', zipfile.ZIP_DEFLATED) as archive:
        for file in sorted(args.stage.rglob('*')):
            if file.is_file():
                archive.write(file, file.relative_to(args.stage).as_posix())
print(f'Validated {filename}, version {version}, {len(lines)} files checksummed')
