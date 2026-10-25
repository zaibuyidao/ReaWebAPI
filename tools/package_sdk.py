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
        'reaper.d.ts', 'runtime-api.d.ts', 'app-manifest.schema.json', 'reaper-api.generated.d.ts', 'reaper.js', 'reaper-api.generated.js', 'browser-files.mjs', 'browser-files.d.mts', 'jsfx-webui.mjs', 'jsfx-webui.d.mts', 'README.md', 'README.zh-CN.md')}
    paths['ReaWebAPI/docs/jsfx-webui.md'] = root / 'docs/jsfx-webui.md'
    paths['ReaWebAPI/docs/jsfx-native-channel.md'] = root / 'docs/jsfx-native-channel.md'
    paths['ReaWebAPI/docs/jsfx-webui.zh-CN.md'] = root / 'docs/jsfx-webui.zh-CN.md'
    paths['ReaWebAPI/docs/jsfx-native-channel.zh-CN.md'] = root / 'docs/jsfx-native-channel.zh-CN.md'
    paths['ReaWebAPI/SDK/jsfx/reaweb_jsfx.inc'] = root / 'runtime/jsfx/reaweb_jsfx.inc'
    for demo in ('analyzer', 'curve', 'resource'):
        directory = root / 'runtime/jsfx/examples' / demo
        for path in directory.iterdir():
            if path.is_file():
                paths[f'ReaWebAPI/SDK/jsfx/examples/{demo}/{path.name}'] = path
        for name in ('jsfx-webui.mjs', 'jsfx-webui.d.mts', 'runtime-api.d.ts'):
            paths[f'ReaWebAPI/SDK/jsfx/examples/{demo}/{name}'] = root / 'runtime' / name
        paths[f'ReaWebAPI/SDK/jsfx/examples/{demo}/reaweb_jsfx.inc'] = root / 'runtime/jsfx/reaweb_jsfx.inc'
    paths['ReaWebAPI/SDK/tools/validate_app.py'] = root / 'tools/validate_app.py'
    paths['ReaWebAPI/docs/browser-media.md'] = root / 'docs/browser-media.md'
    paths['ReaWebAPI/SDK/native/reaweb_service.h'] = root / 'src/public/reaweb_service.h'
    for name in ('reaweb_stream.h', 'reaweb_tasks.h'):
        paths[f'ReaWebAPI/SDK/native/{name}'] = root / 'src/public' / name
    paths['ReaWebAPI/SDK/native/stream_extension.cpp'] = root / 'tests/native_stream_extension.cpp'
    for name in ('native-streams.md', 'native-streams.zh-CN.md'):
        paths[f'ReaWebAPI/docs/{name}'] = root / 'docs' / name
    for name in ('reawebapi-native-stream.lua', 'index.html', 'app.js', 'style.css', 'README.md', 'app.json'):
        paths[f'ReaWebAPI/SDK/native-stream/{name}'] = root / 'web/native-stream' / name
    paths['ReaWebAPI/SDK/native/test_extension.cpp'] = root / 'tests/native_service_extension.cpp'
    for name in ('external-clients.md', 'external-clients.zh-CN.md'):
        paths[f'ReaWebAPI/docs/{name}'] = root / 'docs' / name
    for name in ('index.html', 'app.js', 'style.css', 'README.md'):
        paths[f'ReaWebAPI/SDK/external-client/{name}'] = root / 'web/external-client' / name
    paths['ReaWebAPI/docs/native-services.md'] = root / 'docs/native-services.md'
    paths['ReaWebAPI/docs/native-services.zh-CN.md'] = root / 'docs/native-services.zh-CN.md'
    for name in ('reawebapi-native-service.lua', 'Coexist.lua', 'index.html', 'app.js', 'style.css', 'README.md', 'app.json'):
        paths[f'ReaWebAPI/SDK/native-service/{name}'] = root / 'web/native-service' / name
    for name in ('reawebapi-lua-backend.lua', 'index.html', 'app.js', 'style.css', 'README.md', 'app.json'):
        paths[f'ReaWebAPI/SDK/lua-backend/{name}'] = root / 'web/lua-backend' / name
    for name in ('Open.lua', 'index.html', 'app.js', 'style.css', 'app.json'):
        paths[f'ReaWebAPI/SDK/runtime-demo/{name}'] = root / 'runtime/runtime-demo' / name
    for name in ('Open.lua', 'index.html', 'app.json', 'README.md'):
        paths[f'ReaWebAPI/SDK/hidden-start/{name}'] = root / 'runtime/hidden-start' / name
    for name in ('reawebapi-frameless-resizable.lua', 'reawebapi-frameless-fixed.lua', 'launch.lua', 'index.html', 'app.js', 'style.css', 'app.json', 'README.md'):
        paths[f'ReaWebAPI/SDK/frameless/{name}'] = root / 'web/frameless' / name
    for name in ('reawebapi-transparent.lua', 'index.html', 'app.js', 'style.css', 'app.json', 'README.md'):
        paths[f'ReaWebAPI/SDK/transparent/{name}'] = root / 'web/transparent' / name
    for name in ('Open.lua', 'index.html', 'app.js', 'style.css', 'jsconfig.json', 'app.json'):
        paths[f'ReaWebAPI/SDK/starter/{name}'] = root / 'runtime/starter' / name
    for name in ('Open.lua', 'index.html', 'app.js', 'ui.js', 'style.css', 'README.md', 'README.zh-CN.md', 'app.json',
                 'data/config.json', 'modules/空 格#%.js', 'workers/classic.js', 'workers/module.js'):
        paths[f'ReaWebAPI/SDK/web-runtime/{name}'] = root / 'runtime/web-runtime' / name
    for name in ('README.md', 'README.zh-CN.md', 'Open.lua', 'OpenDev.lua', 'index.html',
                 'package.json', 'package-lock.json', 'tsconfig.json', 'build.mjs',
                 'src/main.ts', 'src/worker.ts', 'src/style.css', 'public/data.json', 'public/app.json'):
        paths[f'ReaWebAPI/SDK/modern/{name}'] = root / 'runtime/modern' / name
    for name in ('README.md', 'README.zh-CN.md', 'development.md', 'development.zh-CN.md',
                 'devtools.md', 'devtools.zh-CN.md', 'SMOKE_TEST.md',
                 'host-api.md', 'host-api.zh-CN.md', 'runtime-api.md', 'runtime-api.zh-CN.md', 'runtime-api-inventory.md', 'permission-design.md', 'api-reference.md', 'frontend.md', 'frontend.zh-CN.md', 'release-notes.md', 'source-layout.md', 'frame-timer-verification.md', 'frame-timer-verification.zh-CN.md'):
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
            for header in ('reaweb_stream.h', 'reaweb_tasks.h'):
                text = text.replace(f'](../src/public/{header})', f'](../SDK/native/{header})')
            text = text.replace('](../web/native-stream/', '](../SDK/native-stream/').replace('](web/native-stream/', '](SDK/native-stream/')
            text = text.replace('](../web/external-client/', '](../SDK/external-client/').replace('](web/external-client/', '](SDK/external-client/')
            text = text.replace('](../web/frameless/', '](../SDK/frameless/').replace('](web/frameless/', '](SDK/frameless/')
            text = text.replace('](../web/transparent/', '](../SDK/transparent/').replace('](web/transparent/', '](SDK/transparent/')
            text = text.replace('](../tests/native_stream_extension.cpp)', '](../SDK/native/stream_extension.cpp)')
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
    files = payload(root, version, revision)
    # Remove the old packaged copy when an install prefix is reused. Source notes stay local.
    (stage / 'ReaWebAPI/docs/VALIDATION.md').unlink(missing_ok=True)
    for name, content in files.items():
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
