"""Embed the ordered WebView injection scripts in a C++ header."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('sources', type=Path, nargs='+')
    args = parser.parse_args()
    script = '(() => {\n' + '\n'.join(path.read_text(encoding='utf-8-sig') for path in args.sources) + '\n})();'
    chunks = '\n'.join('  value += ' + json.dumps(script[i:i+2000]) + ';' for i in range(0, len(script), 2000))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('#pragma once\n#include <string>\nnamespace reaweb {\ninline const std::string bridge_script = [] {\n'
                           '  std::string value;\n' + chunks + '\n  return value;\n}();\n}\n', encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
