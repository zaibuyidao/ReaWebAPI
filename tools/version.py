"""Read the extension version from its single source of truth: CMake project()."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def source_version(root=ROOT):
    cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8-sig')
    match = re.search(r'^\s*project\s*\(\s*ReaWebAPI\s+VERSION\s+([^\s)]+)',
                      cmake, re.MULTILINE | re.IGNORECASE)
    if not match or not re.fullmatch(r'[0-9]+(?:\.[0-9]+){0,3}', match.group(1)):
        raise ValueError('CMakeLists.txt must declare project(ReaWebAPI VERSION <major>[.<minor>[.<patch>[.<tweak>]]])')
    return match.group(1)
