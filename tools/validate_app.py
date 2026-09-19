"""Read-only validation of a minimal ReaWebAPI App manifest and its HTML entry."""
import argparse
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import re


def validate(path):
    path = Path(path).resolve(strict=True)
    if path.stat().st_size > 65536:
        raise ValueError('Manifest exceeds 64 KiB')
    def object_pairs(pairs):
        value = {}
        for key, child in pairs:
            if key in value:
                raise ValueError('Duplicate manifest key: ' + key)
            value[key] = child
        return value
    manifest = json.loads(path.read_text(encoding='utf-8-sig'), object_pairs_hook=object_pairs)
    if not isinstance(manifest, dict) or set(manifest) - {'schemaVersion', 'name', 'version', 'author', 'entry'}:
        raise ValueError('Invalid manifest fields; permissions are not implemented')
    for key in ('name', 'version', 'entry'):
        if not isinstance(manifest.get(key), str) or not manifest[key].strip() or '\0' in manifest[key]:
            raise ValueError('Missing or invalid manifest field: ' + key)
    if len(manifest['name']) > 256 or len(manifest['entry']) > 4096:
        raise ValueError('Manifest name or entry exceeds its limit')
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?', manifest['version']):
        raise ValueError('Expected a major.minor.patch version')
    if type(manifest.get('schemaVersion', 1)) is not int or manifest.get('schemaVersion', 1) != 1:
        raise ValueError('Unsupported manifest schema version')
    if 'author' in manifest and (not isinstance(manifest['author'], str) or len(manifest['author']) > 256):
        raise ValueError('Invalid manifest author')
    entry = manifest['entry']
    relative = PurePosixPath(entry)
    if '\\' in entry or ':' in entry or relative.is_absolute() or PureWindowsPath(entry).drive or '..' in relative.parts:
        raise ValueError('Entry must be a relative forward-slash path within the App')
    target = (path.parent / entry).resolve(strict=True)
    if not target.is_relative_to(path.parent) or not target.is_file() or target.suffix.lower() not in ('.html', '.htm'):
        raise ValueError('Entry must resolve to an HTML file inside the App directory')
    return manifest, target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    args = parser.parse_args()
    try:
        manifest, entry = validate(args.manifest)
    except (OSError, ValueError) as error:
        parser.exit(2, f'Invalid App manifest: {error}\n')
    print(f'Validated {manifest["name"]} {manifest["version"]}: {entry}')


if __name__ == '__main__':
    main()
