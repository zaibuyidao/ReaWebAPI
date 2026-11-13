"""Build a private WebKitGTK runtime with the upstream isolated-process SAB fix."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import urllib.request

VERSION = '2.54.2'
ARCHIVE_SHA256 = 'a76345b391bef6211fb5ab6cfab2e86b285d18cda17cbb9ade595f1252b86065'
URL = f'https://webkitgtk.org/releases/webkitgtk-{VERSION}.tar.xz'
UPSTREAM = '104506cdb2c45aa547db1eb50313abc15473b815'
BEFORE = '#undef WEBKIT_APPLY_JSC_OPTION_FROM_SHARED_PREFERENCE\n        JSC::Options::notifyOptionsChanged();'
AFTER = '#undef WEBKIT_APPLY_JSC_OPTION_FROM_SHARED_PREFERENCE\n#if PLATFORM(GTK) || PLATFORM(WPE)\n        if (parameters.crossOriginMode == WebCore::CrossOriginMode::Isolated)\n            JSC::Options::useSharedArrayBuffer() = true;\n#endif\n        JSC::Options::notifyOptionsChanged();'
PATH_BEFORE = '''#if ENABLE(DEVELOPER_MODE)
    static const char* execDirectory = g_getenv("WEBKIT_EXEC_PATH");'''
PATH_AFTER = '''    static const char* execDirectory = g_getenv("WEBKIT_EXEC_PATH");'''


def stage(source, build, output):
    if AFTER not in (source / 'Source/WebKit/WebProcess/WebProcess.cpp').read_text():
        raise ValueError('Missing isolated-process SharedArrayBuffer patch')
    process_path = (source / 'Source/WebKit/Shared/glib/ProcessExecutablePathGLib.cpp').read_text()
    if PATH_BEFORE in process_path or '#if ENABLE(DEVELOPER_MODE)\n    static String executablePath = getExecutablePath();' not in process_path:
        raise ValueError('Missing runtime relocation patch')
    output.mkdir(parents=True, exist_ok=False)
    files = {
        'lib/libwebkit2gtk-4.1.so.0': next((build / 'lib').glob('libwebkit2gtk-4.1.so.0.*')),
        'lib/libjavascriptcoregtk-4.1.so.0': next((build / 'lib').glob('libjavascriptcoregtk-4.1.so.0.*')),
        'lib/webkit2gtk-4.1/injected-bundle/libwebkit2gtkinjectedbundle.so': build / 'lib/libwebkit2gtkinjectedbundle.so',
    }
    for name in ('WebKitWebProcess', 'WebKitNetworkProcess', 'WebKitGPUProcess'):
        files[f'libexec/webkit2gtk-4.1/{name}'] = build / 'bin' / name
    for relative, original in files.items():
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, target)
        subprocess.run(['strip', '--strip-unneeded', str(target)], check=True)
    for original in source.rglob('*'):
        if original.is_file() and original.name.upper().startswith(('LICENSE', 'COPYING')):
            target = output / 'licenses' / original.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(original, target)
    shutil.copy2(__file__, output / 'build_webkit_runtime.py')
    metadata = {'webkitgtk': VERSION, 'architecture': platform.machine(), 'source': URL,
        'sourceSha256': ARCHIVE_SHA256, 'upstreamPatch': f'https://github.com/WebKit/WebKit/commit/{UPSTREAM}',
        'patch': 'Enable SharedArrayBuffer only in GTK/WPE cross-origin-isolated Web processes.',
        'localPatch': 'Honor WEBKIT_EXEC_PATH in release builds without enabling developer mode.'}
    (output / 'runtime.json').write_text(json.dumps(metadata, indent=2) + '\n')
    (output / 'SOURCE.txt').write_text(
        f'WebKitGTK {VERSION}\nSource: {URL}\nSHA-256: {ARCHIVE_SHA256}\n'
        f'Backport: https://github.com/WebKit/WebKit/commit/{UPSTREAM}\n'
        'The included build_webkit_runtime.py applies the four-line upstream change and a local\n'
        'relocation fix allowing WEBKIT_EXEC_PATH in release builds without developer mode.\n'
        'WebKit retains its upstream licenses. See licenses/ and the source archive.\n')
    lines = [f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(output).as_posix()}'
        for p in sorted(output.rglob('*')) if p.is_file()]
    (output / 'SHA256SUMS.txt').write_text('\n'.join(lines) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--stage-only', action='store_true')
    args = parser.parse_args()
    work = args.work.resolve(); work.mkdir(parents=True, exist_ok=True)
    source, build = work / f'webkitgtk-{VERSION}', work / 'build-clang'
    if not args.stage_only:
        archive = work / f'webkitgtk-{VERSION}.tar.xz'
        if not archive.exists(): urllib.request.urlretrieve(URL, archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
            raise ValueError('WebKitGTK source checksum mismatch')
        if not source.exists():
            with tarfile.open(archive) as stream: stream.extractall(work, filter='data')
        target = source / 'Source/WebKit/WebProcess/WebProcess.cpp'
        text = target.read_text()
        if BEFORE in text:
            assert text.count(BEFORE) == 1
            target.write_text(text.replace(BEFORE, AFTER))
        elif text.count(AFTER) != 1: raise ValueError('Unexpected WebProcess source')
        target = source / 'Source/WebKit/Shared/glib/ProcessExecutablePathGLib.cpp'
        text = target.read_text()
        if PATH_BEFORE in text:
            assert text.count(PATH_BEFORE) == 1
            text=text.replace(PATH_BEFORE,PATH_AFTER).replace(
                '    static String executablePath = getExecutablePath();',
                '#if ENABLE(DEVELOPER_MODE)\n    static String executablePath = getExecutablePath();')
            target.write_text(text)
        elif '#if ENABLE(DEVELOPER_MODE)\n    static String executablePath = getExecutablePath();' not in text:
            raise ValueError('Unexpected process-path source')
        subprocess.run(['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja', '-DPORT=GTK',
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=clang-18', '-DCMAKE_CXX_COMPILER=clang++-18',
            '-DCMAKE_INSTALL_PREFIX=' + str(work / 'install'), '-DUSE_GTK4=OFF', '-DENABLE_DOCUMENTATION=OFF',
            '-DENABLE_INTROSPECTION=OFF', '-DENABLE_MINIBROWSER=OFF', '-DENABLE_BUBBLEWRAP_SANDBOX=ON',
            '-DUSE_LIBBACKTRACE=OFF', '-DUSE_SYSTEM_SYSPROF_CAPTURE=OFF', '-DUSE_SYSTEM_UNIFDEF=OFF'], check=True)
        # The 2.54.2 NetworkProcess target omits an ordering dependency on this generated header.
        subprocess.run(['cmake', '--build', str(build), '--target',
            'WebKitGTK/DerivedSources/WebKitDirectoryInputStreamData.h', '--parallel', str(args.jobs)], check=True)
        subprocess.run(['cmake', '--build', str(build), '--target', 'WebKit', 'WebKitWebProcess',
            'WebKitNetworkProcess', 'WebKitGPUProcess', 'webkit2gtkinjectedbundle', '--parallel', str(args.jobs)], check=True)
    stage(source, build, args.output.resolve())


if __name__ == '__main__': main()
