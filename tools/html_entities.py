"""Generate the HTML character reference tables used to read application titles."""
from html.entities import html5
from pathlib import Path
import sys


def generate(output):
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('w', encoding='ascii', newline='\n') as header:
        header.write('#pragma once\n#include <string_view>\nnamespace reaweb::app_names {\n')
        header.write('struct NamedReference { std::string_view name, value; };\n')
        header.write('inline constexpr NamedReference named_references[] = {\n')
        for name, value in sorted(html5.items()):
            utf8 = ''.join(f'\\x{byte:02x}' for byte in value.encode('utf-8'))
            header.write(f'  {{"{name}", "{utf8}"}},\n')
        header.write('};\n')
        header.write(f'inline constexpr unsigned longest_reference = {max(map(len, html5))};\n')
        # HTML maps numeric references in the C1 range through Windows-1252,
        # retaining the original code point for the undefined entries.
        c1 = [ord(bytes([n]).decode('cp1252', errors='replace')) for n in range(0x80, 0xa0)]
        values = [hex(n if cp == 0xfffd else cp) for n, cp in enumerate(c1, 0x80)]
        header.write('inline constexpr unsigned c1_references[] = {' + ', '.join(values) + '};\n}\n')


if __name__ == '__main__':
    generate(Path(sys.argv[1]))
