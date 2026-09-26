"""Stage the architecture-independent SDK/docs or create its standalone release ZIP."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import zipfile

if not __package__:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.api_sync.generate import verify
from tools.version import source_version

ROOT = Path(__file__).resolve().parents[1]


def payload(root, version, revision):
    schema = json.loads((root / 'api/reaper_api.json').read_text(encoding='utf-8-sig'))
    bindings = json.loads((root / 'api/bindings.json').read_text(encoding='utf-8-sig'))
    verify(root, schema, bindings)
    paths = {f'ReaWebAPI/SDK/{name}': root / 'runtime' / name for name in (
        'reaper.d.ts', 'runtime-api.d.ts', 'app-manifest.schema.json', 'reaper-api.generated.d.ts', 'reaper.js', 'reaper-api.generated.js', 'README.md', 'README.zh-CN.md')}
    paths['ReaWebAPI/SDK/tools/validate_app.py'] = root / 'tools/validate_app.py'
    paths['ReaWebAPI/SDK/native/reaweb_service.h'] = root / 'src/public/reaweb_service.h'
    paths['ReaWebAPI/SDK/native/test_extension.cpp'] = root / 'tests/native_service_extension.cpp'
    paths['ReaWebAPI/docs/native-services.md'] = root / 'docs/native-services.md'
    paths['ReaWebAPI/docs/native-services.zh-CN.md'] = root / 'docs/native-services.zh-CN.md'
    for name in ('Open.lua', 'Coexist.lua', 'index.html', 'app.js', 'style.css', 'README.md'):
        paths[f'ReaWebAPI/SDK/native-service/{name}'] = root / 'web/native-service' / name
    for name in ('Open.lua', 'index.html', 'app.js', 'style.css', 'README.md'):
        paths[f'ReaWebAPI/SDK/lua-backend/{name}'] = root / 'web/lua-backend' / name
    for name in ('Open.lua', 'index.html', 'app.js', 'style.css', 'app.json'):
        paths[f'ReaWebAPI/SDK/runtime-demo/{name}'] = root / 'runtime/runtime-demo' / name
    for name in ('Open.lua', 'index.html', 'app.js', 'style.css', 'jsconfig.json'):
        paths[f'ReaWebAPI/SDK/starter/{name}'] = root / 'runtime/starter' / name
    for name in ('Open.lua', 'index.html', 'app.js', 'ui.js', 'style.css', 'README.md', 'README.zh-CN.md',
                 'data/config.json', 'modules/空 格#%.js', 'workers/classic.js', 'workers/module.js'):
        paths[f'ReaWebAPI/SDK/web-runtime/{name}'] = root / 'runtime/web-runtime' / name
    for name in ('README.md', 'README.zh-CN.md', 'Open.lua', 'OpenDev.lua', 'index.html',
                 'package.json', 'package-lock.json', 'tsconfig.json', 'build.mjs',
                 'src/main.ts', 'src/worker.ts', 'src/style.css', 'public/data.json'):
        paths[f'ReaWebAPI/SDK/modern/{name}'] = root / 'runtime/modern' / name
    for name in ('README.md', 'README.zh-CN.md', 'development.md', 'development.zh-CN.md',
                 'devtools.md', 'devtools.zh-CN.md', 'SMOKE_TEST.md', 'VALIDATION.md',
                 'host-api.md', 'host-api.zh-CN.md', 'runtime-api.md', 'runtime-api.zh-CN.md', 'runtime-api-inventory.md', 'permission-design.md', 'api-reference.md', 'frontend.md', 'frontend.zh-CN.md', 'release-notes.md', 'source-layout.md'):
        paths[f'ReaWebAPI/docs/{name}'] = root / 'docs' / name
    for name in ('reaper_api.json', 'bindings.json'):
        paths[f'ReaWebAPI/SDK/api/{name}'] = root / 'api' / name
    for name in ('README.md', 'README.zh-CN.md'):
        paths[f'ReaWebAPI/api/{name}'] = root / 'api' / name
    for name in ('README.md', 'README.zh-CN.md', 'THIRD_PARTY.md', 'LICENSE.md', 'COPYING', 'COPYING.LESSER'):
        paths[f'ReaWebAPI/{name}'] = root / name
    files = {}
    for name, path in paths.items():
        content = path.read_bytes()
        if path.suffix == '.md':
            text = content.decode('utf-8-sig')
            text = text.replace('](../runtime/', '](../SDK/').replace('](runtime/', '](SDK/')
            text = text.replace('](../web/lua-backend/', '](../SDK/lua-backend/').replace('](web/lua-backend/', '](SDK/lua-backend/')
            text = text.replace('](../web/native-service/', '](../SDK/native-service/').replace('](web/native-service/', '](SDK/native-service/')
            text = text.replace('](../src/public/reaweb_service.h)', '](../SDK/native/reaweb_service.h)')
            text = text.replace('](../tests/native_service_extension.cpp)', '](../SDK/native/test_extension.cpp)')
            if name == 'ReaWebAPI/docs/release-notes.md':
                text = re.sub(r'\A# ReaWebAPI[ \t]*\r?\n', f'# ReaWebAPI v{version}\n', text, count=1)
            content = text.encode('utf-8')
        files[name] = content
    metadata = dict(version=version, revision=revision, reaperVersion=schema['source']['reaperVersion'],
                    catalogueHash=schema['catalogueHash'], bindings=len(bindings['functions']))
    files['ReaWebAPI/SDK/BUILD.json'] = (json.dumps(metadata, indent=2) + '\n').encode()
    return files


def stage_sdk(stage, version, revision='local', root=ROOT):
    for name, content in payload(root, version, revision).items():
        path = stage / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)


def build_sdk(directory, version, revision='local', root=ROOT):
    files = payload(root, version, revision)
    files['SHA256SUMS.txt'] = ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n'
                                    for name, data in sorted(files.items())).encode('utf-8')
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f'ReaWebAPI-SDK-v{version}.zip'
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name)
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument('--stage', type=Path, help='Merge SDK/docs into an install prefix')
    target.add_argument('--output', type=Path, help='Write the standalone SDK ZIP to this directory')
    parser.add_argument('--revision', default='local')
    args = parser.parse_args()
    version = source_version()
    if args.stage:
        stage_sdk(args.stage, version, args.revision)
        print(f'Staged SDK and documentation for v{version}')
    else:
        print(build_sdk(args.output, version, args.revision))


if __name__ == '__main__':
    main()
