"""Remove retired ReaWebAPI files from a staging tree or a verified package upgrade."""
import argparse
import hashlib
from pathlib import Path
from zipfile import ZipFile

RETIRED = (
    'ReaWebAPI/SDK/reaper-api.generated.js', 'ReaWebAPI/SDK/reaper-api.generated.d.ts',
    'ReaWebAPI/SDK/api/bindings.json', 'ReaWebAPI/SDK/api/reaper_api.json',
    'ReaWebAPI/api/bindings.json', 'ReaWebAPI/api/reaper_api.json',
    'ReaWebAPI/api/README.md', 'ReaWebAPI/api/README.zh-CN.md', 'ReaWebAPI/docs/api-reference.md',
)


def clean_stage(root, previous=None):
    root = Path(root).resolve(strict=True)
    kept = []
    installed = {}
    receipt = root / 'SHA256SUMS.txt'
    if previous is None and receipt.is_file() and receipt.resolve() == receipt:
        for line in receipt.read_text(encoding='utf-8').splitlines():
            digest, _, name = line.partition('  ')
            installed[name] = digest
    for name in RETIRED:
        path = root / name
        if not path.exists() and not path.is_symlink():
            continue
        if path.resolve() != path or not path.is_file():
            kept.append(name)
            continue
        if previous is not None:
            if name not in previous.namelist() or previous.read(name) != path.read_bytes():
                kept.append(name)
                continue
        elif installed.get(name) != hashlib.sha256(path.read_bytes()).hexdigest():
            kept.append(name)
            continue
        path.unlink()
    for name in ('ReaWebAPI/SDK/api', 'ReaWebAPI/api'):
        folder = root / name
        if folder.resolve() == folder and folder.is_dir() and not any(folder.iterdir()):
            folder.rmdir()
    return kept


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--resource-root', required=True, type=Path)
    parser.add_argument('--previous-package', required=True, type=Path)
    args = parser.parse_args()
    with ZipFile(args.previous_package) as archive:
        for name in clean_stage(args.resource_root, archive):
            print('Preserved', name)
