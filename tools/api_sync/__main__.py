"""Run with python -m tools.api_sync, or python tools/api_sync."""
import argparse
import re
import sys
from pathlib import Path

if not __package__:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.api_sync.sources import OFFICIAL_URL, load_source, read_json
from tools.api_sync.model import normalize, compare, coverage, encode, fingerprint, validate_schema, validate_bindings
from tools.api_sync.generate import sdk_files, verify, runtime_header, write_files, report_text

ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return read_json(path.read_text(encoding='utf-8-sig'))


def make_report(old, new, manifest):
    return {'reaperVersion': new['source']['reaperVersion'], 'source': new['source'],
            'catalogueHash': new['catalogueHash'], 'difference': compare(old, new),
            'coverage': coverage(new, manifest),
            'limitations': {n: b['limitations'] for n, b in manifest['functions'].items()}}


def main(argv=None):
    parser = argparse.ArgumentParser(description='Maintain the REAPER API catalogue without changing native bindings or Git.')
    parser.add_argument('--root', type=Path, default=ROOT, help='ReaWebAPI source repository')
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('check', 'update', 'report'):
        command = commands.add_parser(name)
        command.add_argument('--source', help='Official HTML, exchange Markdown/JSON, or HTTPS URL')
        command.add_argument('--format', choices=['auto', 'html', 'markdown', 'json'], default='auto')
        command.add_argument('--source-version')
        command.add_argument('--sha256', help='Require this exact source SHA-256')
        command.add_argument('--offline', action='store_true', help='Use the committed schema without network access')
        command.add_argument('--json', action='store_true', help='Write a machine-readable report to stdout')
        command.add_argument('--full', action='store_true', help='Include all missing bindings (always present in JSON)')
        if name == 'check':
            command.add_argument('--require-complete', action='store_true', help='Also fail for missing or partially compatible bindings')
        if name == 'update':
            command.add_argument('--allow-removals', action='store_true', help='Accept removal of previously catalogued APIs')
            command.add_argument('--allow-downgrade', action='store_true', help='Accept an older source version')
    commands.add_parser('verify', help='Offline schema, native registry and generated SDK validation')
    export = commands.add_parser('export-runtime', help='Emit validated embedded data for CMake (no handlers)')
    export.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    root = args.root.resolve()
    schema_path, bindings_path = root / 'api/reaper_api.json', root / 'api/bindings.json'
    old = read(schema_path) if schema_path.exists() else {}
    manifest = read(bindings_path)
    validate_bindings(manifest)
    if args.command in ('verify', 'export-runtime'):
        verify(root, old, manifest)
        if args.command == 'export-runtime':
            output = args.output.resolve()
            # Export is a build product. Keep it out of source and manifest paths.
            if output.suffix != '.hpp' or output.is_relative_to(root / 'src'):
                raise ValueError('Export must be an .hpp build output outside src/')
            write_files({output: runtime_header(old, manifest)})
        print(f"Verified {len(old['functions'])} definitions and {len(manifest['functions'])} reviewed bindings")
        return 0
    if args.offline and (args.source or args.source_version or args.sha256 or args.format != 'auto'):
        raise ValueError('--offline cannot be combined with source options')
    if old:
        validate_schema(old)
    source_options = args.source or args.source_version or args.sha256 or args.format != 'auto'
    offline = args.offline or (args.command == 'report' and not source_options)
    if offline:
        if not old:
            raise ValueError('No committed schema exists for offline use')
        new = old
    else:
        source, functions, excluded = load_source(args.source or OFFICIAL_URL, args.format, args.source_version, args.sha256)
        new = normalize(source, functions, excluded)
        # The same bytes read from a local cache retain committed provenance.
        if old and source['sha256'] == old['source']['sha256'] and source['format'] == old['source']['format']:
            new['source'] = old['source']
    report = make_report(old, new, manifest)
    difference = report['difference']
    if args.command == 'update':
        if difference['removed'] and not args.allow_removals:
            raise ValueError('Source removes APIs. Inspect check --full, then use --allow-removals if intentional.')
        if old:
            numeric = lambda v: tuple(map(int, re.match(r'(\d+)\.(\d+)', v).groups()))
            if numeric(new['source']['reaperVersion']) < numeric(old['source']['reaperVersion']) and not args.allow_downgrade:
                raise ValueError('Source version is older. Use --allow-downgrade if intentional.')
        output = {schema_path: encode(new)}
        output.update({root / name: content for name, content in sdk_files(manifest, new).items()})
        if old != new:
            version = new['source']['reaperVersion']
            changelog = root / 'api/changelog' / (version + '.md')
            existing = changelog.read_text(encoding='utf-8') if changelog.exists() else f'# REAPER {version} API catalogue\n'
            event = fingerprint({'before': old.get('catalogueHash'), 'after': new['catalogueHash'], 'source': new['source']})
            marker = f'<!-- sync:{event} -->'
            if marker not in existing:
                text = report_text(report, False)
                existing += f'\n{marker}\n\nSource SHA-256: `{new["source"]["sha256"]}`\n\n```text\n{text}```\n'
            output[changelog] = existing
        changed = write_files(output)
        report['written'] = [path.relative_to(root).as_posix() for path in changed]
    if args.json:
        print(encode(report), end='')
    else:
        print(report_text(report, args.full or args.command == 'report'), end='')
        if args.command == 'update':
            print('Updated: ' + (', '.join(report['written']) or 'nothing (already current)'))
    if args.command == 'check':
        drift = old != new or bool(report['coverage']['needsReview'])
        if args.require_complete:
            drift |= bool(report['coverage']['missing'] or report['coverage']['partial'])
        return 1 if drift else 0
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError, TypeError, AttributeError, RecursionError) as error:
        print(f'api_sync: {error}', file=sys.stderr)
        raise SystemExit(2)
