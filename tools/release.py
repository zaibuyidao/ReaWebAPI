"""Assemble ReaPack binaries and the Demo, then optionally publish a complete release."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from urllib.parse import quote
import zipfile

if not __package__:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.package_sdk import build_sdk
from tools.version import source_version

TARGETS = [
    ('windows', 'x64', 'win64', 'reaper_reawebapi-x64.dll'),
    ('macos', 'arm64', 'darwin-arm64', 'reaper_reawebapi-arm64.dylib'),
    ('macos', 'x86_64', 'darwin64', 'reaper_reawebapi-x86_64.dylib'),
    ('linux', 'x86_64', 'linux64', 'reaper_reawebapi-x86_64.so'),
    ('linux', 'aarch64', 'linux-aarch64', 'reaper_reawebapi-aarch64.so'),
]


def native_files():
    for platform, arch, reapack, binary in TARGETS:
        yield reapack, binary
        if platform == 'linux':
            yield reapack, f'reawebapi-webview-{arch}'


def version_notes(version):
    text = (Path(__file__).resolve().parents[1] / 'docs/release-notes.md').read_text(encoding='utf-8')
    # The unversioned heading describes this checkout. Published titles and
    # packaged notes receive their version from CMake, without a second edit.
    match = re.search(r'^# ReaWebAPI(?: v' + re.escape(version) + r')?[ \t]*\r?\n(.*?)(?=^# |\Z)',
                      text, re.MULTILINE | re.DOTALL)
    if not match:
        raise ValueError(f'Missing release notes for v{version}')
    return match.group(1).strip()


def release_body(repo, version):
    base = f'https://github.com/{repo}/releases/download/v{version}'
    platforms = [('Windows x64', 'windows-x64'), ('macOS ARM64', 'macos-arm64'),
                 ('macOS Intel', 'macos-x86_64'), ('Linux x64', 'linux-x86_64'),
                 ('Linux ARM64', 'linux-aarch64')]
    downloads = '\n'.join(f'- [{label}]({base}/ReaWebAPI-{target}-v{version}.zip)' for label, target in platforms)
    return (version_notes(version) + '\n\n## Downloads / 下载\n\n' + downloads + '\n\n' +
            f'[SDK]({base}/ReaWebAPI-SDK-v{version}.zip) · '
            f'[ReaPack]({base}/ReaWebAPI-ReaPack-v{version}.zip) · '
            f'[SHA-256]({base}/SHA256SUMS.txt)\n\n'
            'Platform packages include the extension, SDK and examples. Extract into the REAPER resource directory and restart REAPER. '
            'On Linux, keep the matching WebKit helper beside the extension with execute permission.\n\n'
            '平台安装包包含扩展、SDK 和示例。解压到 REAPER 资源目录后重启。'
            'Linux 的同架构 WebKit 辅助程序与扩展放在同一目录，并赋予执行权限。\n\n'
            'The SDK provides type declarations and templates. The ReaPack package provides binaries, the complete Demo and repository metadata.\n\n'
            'SDK 提供类型声明和开发模板，ReaPack 包提供二进制文件、完整 Demo 和仓库索引元数据。\n\n'
            f'[Documentation / 文档](https://github.com/{repo}#readme)\n')


def demo_entries(directory):
    if not directory.is_dir():
        raise ValueError(f'Missing Demo directory: {directory}')
    return sorted(directory.rglob('*'))


def descriptor(version, demo_directory=None):
    if demo_directory is None:
        demo_directory = Path(__file__).resolve().parents[1] / 'web'
    entries = demo_entries(demo_directory)
    if not (demo_directory / 'ReaWebAPI_Demo.lua').is_file():
        raise ValueError('Missing Demo launcher: ReaWebAPI_Demo.lua')
    lines = ['@description ReaWebAPI: JavaScript Runtime for REAPER', f'@version {version}', '@author zaibuyidao',
             '@link https://github.com/zaibuyidao/ReaScripts/tree/master/ReaWebAPI', '@provides']
    for platform, name in native_files():
        url = f'https://raw.githubusercontent.com/zaibuyidao/ReaScripts/$commit/ReaWebAPI/extension/{name}'
        lines.append(f'  [{platform} extension] {name} {url}')
    for path in entries:
        if not path.is_file():
            continue
        name = path.relative_to(demo_directory).as_posix()
        # An .ext package defaults to extension targets, so the launcher also
        # needs an explicit script type for Action List registration.
        options = 'script main' if name in ('ReaWebAPI_Demo.lua', 'lua-backend/Open.lua', 'native-service/Open.lua', 'native-service/Coexist.lua') else 'script nomain'
        url = f'https://raw.githubusercontent.com/zaibuyidao/ReaScripts/$commit/ReaWebAPI/web/{quote(name, safe="/")}'
        lines.append(f'  [{options}] web/{name} {url}')
    changes = re.split(r'^## (?:更新|简体中文)\s*$', version_notes(version), maxsplit=1, flags=re.MULTILINE)[0]
    lines += ['@changelog'] + ['  ' + line for line in re.findall(r'^- (.+)$', changes, re.MULTILINE)]
    return '\n'.join(lines) + '\n'


def add_demo(archive, directory):
    entries = demo_entries(directory)
    archive.write(directory, 'web/')
    for path in entries:
        archive.write(path, 'web/' + path.relative_to(directory).as_posix())


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
        add_demo(archive, Path(__file__).resolve().parents[1] / 'web')
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


def find_release(repo, tag):
    try:
        return json.loads(gh('api', f'repos/{repo}/releases/tags/{tag}'))
    except subprocess.CalledProcessError as error:
        if 'HTTP 404' not in (error.stderr or ''):
            raise
    # The by-tag endpoint can omit drafts. Reuse a draft left by an interrupted upload.
    pages = json.loads(gh('api', f'repos/{repo}/releases?per_page=100', '--paginate', '--slurp'))
    return next((item for page in pages for item in page if item['tag_name'] == tag), None)


def publish_once(repo, version, revision, assets):
    tag = f'v{version}'
    release = find_release(repo, tag)
    if release and not release['draft']:
        print(f'{tag} is already published. Increment the version for new binaries.')
        return
    # Refuse to attach new binaries to an existing tag for a different commit.
    try:
        tagged = json.loads(gh('api', f'repos/{repo}/commits/{tag}'))['sha']
    except subprocess.CalledProcessError as error:
        if 'HTTP 404' not in (error.stderr or '') and 'HTTP 422' not in (error.stderr or ''):
            raise
        tagged = revision
    if tagged != revision or (release and release['target_commitish'] != revision):
        raise ValueError(f'{tag} points to a different commit. Use a new version.')
    notes = assets[0].parent / 'release-notes.md'
    notes.write_text(release_body(repo, version), encoding='utf-8', newline='\n')
    if not release:
        gh('release', 'create', tag, '--repo', repo, '--target', revision, '--draft',
           '--title', f'ReaWebAPI {tag}', '--notes-file', str(notes))
    else:
        gh('release', 'edit', tag, '--repo', repo, '--title', f'ReaWebAPI {tag}', '--notes-file', str(notes))
    gh('release', 'upload', tag, '--repo', repo, '--clobber', *(str(file) for file in assets))
    # Publish only after every expected asset has been uploaded successfully.
    gh('release', 'edit', tag, '--repo', repo, '--draft=false', '--latest')
    print(f'Published https://github.com/{repo}/releases/tag/{tag}')


def publish(repo, version, revision, assets):
    tag = f'v{version}'
    ref = os.environ.get('GITHUB_REF', '')
    if ref.startswith('refs/tags/') and ref != f'refs/tags/{tag}':
        raise ValueError(f'Tag {ref} does not match CMake version {tag}')
    for attempt in range(3):
        try:
            return publish_once(repo, version, revision, assets)
        except subprocess.CalledProcessError as error:
            detail = (error.stderr or error.stdout or str(error)).strip()
            print(f'GitHub release attempt {attempt + 1} failed: {detail}', file=sys.stderr, flush=True)
            transient = re.search(r'HTTP (?:429|5\d\d)\b|timed? out|timeout|connection reset|'
                                  r'connection refused|unexpected EOF|TLS handshake|temporary failure', detail, re.I)
            # Re-read release state before retrying: the server may have accepted a failed request.
            if attempt == 2 or not transient:
                raise
            delay = 5 * (attempt + 1)
            print(f'Retrying release publication in {delay}s.', file=sys.stderr, flush=True)
            time.sleep(delay)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--publish', action='store_true')
    parser.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY'))
    args = parser.parse_args()
    version = source_version()
    assets = assemble(args.assets, version, args.revision)
    print(f'Prepared v{version}: {len(assets)} release assets', flush=True)
    if args.publish:
        if not args.repo:
            parser.error('--repo is required when publishing')
        publish(args.repo, version, args.revision, assets)


if __name__ == '__main__':
    main()
