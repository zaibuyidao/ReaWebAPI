"""Assemble the seven-file ReaPack payload, then optionally publish a complete release."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zipfile

if not __package__:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.package_sdk import build_sdk

TARGETS = [
    ('windows', 'x64', 'win64', 'reaper_reawebapi-x64.dll'),
    ('macos', 'arm64', 'darwin-arm64', 'reaper_reawebapi-arm64.dylib'),
    ('macos', 'x86_64', 'darwin64', 'reaper_reawebapi-x86_64.dylib'),
    ('linux', 'x86_64', 'linux64', 'reaper_reawebapi-x86_64.so'),
    ('linux', 'aarch64', 'linux-aarch64', 'reaper_reawebapi-aarch64.so'),
]


def source_version():
    cmake = (Path(__file__).resolve().parent.parent / 'CMakeLists.txt').read_text(encoding='utf-8-sig')
    return re.search(r'project\(ReaWebAPI VERSION (\d+\.\d+\.\d+)', cmake).group(1)


def native_files():
    for platform, arch, reapack, binary in TARGETS:
        yield reapack, binary
        if platform == 'linux':
            yield reapack, f'reawebapi-webview-{arch}'


def descriptor(version):
    lines = ['@description ReaWebAPI', f'@version {version}', '@author zaibuyidao',
             '@link https://github.com/zaibuyidao/ReaScripts/tree/master/ReaWebAPI', '@provides']
    for platform, name in native_files():
        url = f'https://raw.githubusercontent.com/zaibuyidao/ReaScripts/$commit/ReaWebAPI/extension/{name}'
        lines.append(f'  [{platform} extension] {name} {url}')
    lines += ['@changelog', '  Bind all 730 standard REAPER APIs with typed native dispatch.',
              '  Support handles, multiple results, binary MIDI and audio sample buffers.',
              '  Provide a separate SDK, starter and bilingual developer documentation.',
              '  Fix Linux large messages and cross-project batch Undo ownership.',
              '  Add 173-method batches, managed Undo, file/desktop APIs and more events.',
              '  Add a Vite/TypeScript template and loopback development entry.']
    return '\n'.join(lines) + '\n'


def assemble(directory, version, revision):
    assets = []
    for platform, arch, _, binary in TARGETS:
        files = [binary] + ([f'reawebapi-webview-{arch}'] if platform == 'linux' else [])
        bundle = directory / f'ReaWebAPI-{platform}-{arch}-v{version}.zip'
        with zipfile.ZipFile(bundle) as archive:
            metadata = json.loads(archive.read('ReaWebAPI/BUILD.json'))
            expected = dict(version=version, platform=platform, architecture=arch, revision=revision, extension=binary)
            if metadata != expected:
                raise ValueError(f'Build metadata mismatch: {bundle.name}')
            for name in files:
                file = directory / name
                if not file.is_file() or not file.stat().st_size or archive.read(f'UserPlugins/{name}') != file.read_bytes():
                    raise ValueError(f'Missing or mismatched native asset: {name}')
                assets.append(file)
        assets.append(bundle)
    ext = directory / 'ReaWebAPI.ext'
    ext.write_text(descriptor(version), encoding='utf-8', newline='\n')
    reapack = directory / f'ReaWebAPI-ReaPack-v{version}.zip'
    with zipfile.ZipFile(reapack, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.write(ext, ext.name)
        for _, name in native_files():
            # Preserve executable permission even when Actions normalized downloaded files to 0644.
            info = zipfile.ZipInfo(f'extension/{name}')
            info.create_system = 3
            info.external_attr = 0o100755 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, (directory / name).read_bytes())
    assets += [ext, reapack, build_sdk(directory, version, revision)]
    checksums = directory / 'SHA256SUMS.txt'
    checksums.write_text(''.join(f'{hashlib.sha256(file.read_bytes()).hexdigest()}  {file.name}\n'
                                for file in sorted(assets)), encoding='ascii')
    return assets + [checksums]


def gh(*args):
    return subprocess.run(['gh', *args], text=True, capture_output=True, encoding='utf-8', check=True).stdout


def publish(repo, version, revision, assets):
    tag = f'v{version}'
    ref = os.environ.get('GITHUB_REF', '')
    if ref.startswith('refs/tags/') and ref != f'refs/tags/{tag}':
        raise ValueError(f'Tag {ref} does not match CMake version {tag}')
    try:
        release = json.loads(gh('api', f'repos/{repo}/releases/tags/{tag}'))
    except subprocess.CalledProcessError as error:
        if 'HTTP 404' not in error.stderr:
            raise
        release = None
    if release and not release['draft']:
        print(f'{tag} is already published. Increment the version for new binaries.')
        return
    # Refuse to attach new binaries to an existing tag for a different commit.
    try:
        tagged = json.loads(gh('api', f'repos/{repo}/commits/{tag}'))['sha']
    except subprocess.CalledProcessError as error:
        if 'HTTP 404' not in error.stderr and 'HTTP 422' not in error.stderr:
            raise
        tagged = revision
    if tagged != revision or (release and release['target_commitish'] != revision):
        raise ValueError(f'{tag} points to a different commit. Use a new version.')
    notes = assets[0].parent / 'release-notes.md'
    changes = (Path(__file__).resolve().parents[1] / 'docs/release-notes.md').read_text(encoding='utf-8')
    notes.write_text(changes + '\n\n' +
        'Native files can be downloaded individually. Linux needs both the .so and its matching WebKit helper.\n\n'
        'Platform ZIPs include the demo, SDK and developer documentation. The standalone SDK ZIP adds a runnable starter and the complete API reference. '
        'The ReaPack ZIP contains only extension/ (seven files) and ReaWebAPI.ext. '
        'Copy it into ReaScripts/ReaWebAPI before indexing that repository.\n\n'
        '各平台扩展均提供单独下载。Linux 还需同架构的 WebKit 辅助程序。\n\n'
        '平台 ZIP 包含 Demo、SDK 和开发文档。独立 SDK ZIP 提供最小模板及完整 API 参考。ReaPack ZIP 仅含 extension/ 内的 7 个文件与 ReaWebAPI.ext。\n', encoding='utf-8')
    if not release:
        gh('release', 'create', tag, '--repo', repo, '--target', revision, '--draft',
           '--title', f'ReaWebAPI {tag}', '--notes-file', str(notes))
    gh('release', 'upload', tag, '--repo', repo, '--clobber', *(str(file) for file in assets))
    # Publish only after every expected asset has been uploaded successfully.
    gh('release', 'edit', tag, '--repo', repo, '--draft=false', '--latest')
    print(f'Published https://github.com/{repo}/releases/tag/{tag}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--publish', action='store_true')
    parser.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY'))
    args = parser.parse_args()
    version = source_version()
    assets = assemble(args.assets, version, args.revision)
    print(f'Prepared v{version}: {len(assets)} release assets, 8 files in the ReaPack ZIP')
    if args.publish:
        if not args.repo:
            parser.error('--repo is required when publishing')
        publish(args.repo, version, args.revision, assets)


if __name__ == '__main__':
    main()
