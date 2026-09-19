"""Canonical schema, reviewed bindings, comparison and coverage."""
import hashlib
import json
import re
from . import TOOL_VERSION
from .sources import OFFICIAL_URL

SCHEMA_VERSION = 1
CATEGORIES = ['Track', 'Project', 'Item', 'Take', 'FX', 'Envelope', 'Send', 'Marker', 'Transport', 'MIDI', 'Utility']


def encode(value):
    return json.dumps(value, ensure_ascii=False, indent=2) + '\n'


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode()).hexdigest()


def category(name):
    for pattern, label in [(r'MIDI|midi', 'MIDI'), (r'FX|Fx', 'FX'), (r'Send|Receive|Routing', 'Send'),
                           (r'Envelope|AutomationItem', 'Envelope'), (r'Marker|Region', 'Marker'),
                           (r'Take', 'Take'), (r'Item', 'Item'), (r'Track', 'Track'),
                           (r'Play|Stop|Pause|Record|Transport|Repeat', 'Transport'),
                           (r'Project|Proj|Undo', 'Project')]:
        if re.search(pattern, name):
            return label
    return 'Utility'


def lua_values(text, returns=False):
    values = []
    for i, token in enumerate(text.split(',') if text.strip() else []):
        match = re.fullmatch(r'\s*(optional\s+)?([A-Za-z_][\w.]*)(?:\s+([A-Za-z_][\w.]*))?\s*', token)
        if not match or (not returns and not match[3]):
            raise ValueError(f'Unrecognized Lua value: {token!r}')
        values.append({'name': match[3] or ('retval' if i == 0 else f'result{i + 1}'),
                       'type': match[2], 'optional': bool(match[1])})
    if len({x['name'] for x in values}) != len(values):
        raise ValueError('Duplicate Lua value names')
    return values


def normalize_function(name, raw):
    if not re.fullmatch(r'[A-Za-z_]\w*', name) or name.startswith('ReaWeb'):
        raise ValueError(f'Invalid or reserved API name: {name}')
    signatures = raw.get('signatures', {})
    if set(signatures) != {'c', 'lua'} or any(not isinstance(v, str) for v in signatures.values()):
        raise ValueError(f'{name}: expected C and Lua signatures')
    signatures = {lang: ' '.join(signatures[lang].split()).rstrip(';') for lang in ('c', 'lua')}
    c = re.fullmatch(r'(.+?)\s+' + re.escape(name) + r'\((.*)\)', signatures['c'])
    lua = re.fullmatch(r'(.*?)reaper\.' + re.escape(name) + r'\((.*)\)', signatures['lua'])
    if not c or not lua:
        raise ValueError(f'{name}: signature/name mismatch')
    native_params = []
    for token in c[2].split(',') if c[2] and c[2] != 'void' else []:
        match = re.fullmatch(r'\s*(.+?[\s*&])([A-Za-z_]\w*)\s*', token)
        if not match:
            raise ValueError(f'{name}: unrecognized C parameter {token!r}')
        native_params.append({'name': match[2], 'type': match[1].strip()})
    returns_text = lua[1].strip()
    if returns_text.endswith('='):
        returns_text = returns_text[:-1].strip()
    value = {'category': category(name), 'signatures': signatures,
             'parameters': lua_values(lua[2]), 'returns': lua_values(returns_text, True),
             'native': {'parameters': native_params, 'returnType': c[1]},
             'documentation': OFFICIAL_URL + '#' + name}
    value['signatureHash'] = fingerprint({k: value[k] for k in ('parameters', 'returns', 'native')})
    if raw.get('documentationHash') is not None:
        if not re.fullmatch(r'[0-9a-f]{64}', raw['documentationHash']):
            raise ValueError(f'{name}: invalid documentation hash')
        value['documentationHash'] = raw['documentationHash']
    return value


def normalize(source, raw, excluded):
    if (source.get('scope') != 'standard-reascript-c-lua'
            or source.get('format') not in ('html', 'markdown', 'json')
            or not re.fullmatch(r'\d+\.\d+[A-Za-z0-9.+_-]*', source.get('reaperVersion', ''))
            or not re.fullmatch(r'[0-9a-f]{64}', source.get('sha256', ''))
            or not isinstance(source.get('origin'), str) or not source['origin']
            or source.get('documentation') != OFFICIAL_URL):
        raise ValueError('Invalid source metadata')
    if not isinstance(excluded, list) or any(not isinstance(name, str) for name in excluded):
        raise ValueError('Invalid excluded builtin list')
    functions = {}
    for name in sorted(raw):
        try:
            functions[name] = normalize_function(name, raw[name])
        except (ValueError, TypeError, AttributeError) as error:
            raise ValueError(f'{name}: {error}') from error
    if not functions:
        raise ValueError('Empty API schema')
    result = {'schemaVersion': SCHEMA_VERSION, 'generatorVersion': TOOL_VERSION,
              'source': source, 'excludedBuiltins': sorted(set(excluded)), 'functions': functions}
    result['catalogueHash'] = fingerprint(functions)
    return result


def validate_schema(schema):
    if schema.get('schemaVersion') != SCHEMA_VERSION or schema.get('generatorVersion') != TOOL_VERSION:
        raise ValueError('Unsupported schema/generator version')
    if normalize(schema['source'], schema['functions'], schema['excludedBuiltins']) != schema:
        raise ValueError('Schema is not canonical or its hashes/derived fields are stale. Run update from the original source.')


def js_type_names(value):
    """Small declarative TS grammar: named types, numeric literals, unions, tuples, arrays."""
    if not isinstance(value, str):
        raise ValueError('JavaScript type must be a string')
    tokens = re.findall(r'[A-Za-z_]\w*|\d+|[\[\],|]|\S', value)
    index, names = 0, set()

    def union():
        nonlocal index
        atom()
        while index < len(tokens) and tokens[index] == '|':
            index += 1
            atom()

    def atom():
        nonlocal index
        if index >= len(tokens):
            raise ValueError(f'Incomplete JavaScript type: {value}')
        token = tokens[index]
        index += 1
        if token == '[':
            if index < len(tokens) and tokens[index] != ']':
                union()
                while index < len(tokens) and tokens[index] == ',':
                    index += 1
                    union()
            if index >= len(tokens) or tokens[index] != ']':
                raise ValueError(f'Invalid tuple type: {value}')
            index += 1
        elif re.fullmatch(r'[A-Za-z_]\w*', token):
            names.add(token)
        elif not token.isdigit():
            raise ValueError(f'Invalid JavaScript type: {value}')
        while tokens[index:index + 2] == ['[', ']']:
            index += 2

    union()
    if index != len(tokens):
        raise ValueError(f'Unexpected JavaScript type tokens: {value}')
    return names


def validate_bindings(manifest):
    if manifest.get('bindingVersion') != 1 or not isinstance(manifest.get('functions'), dict):
        raise ValueError('Unsupported binding manifest')
    for name, entry in manifest['functions'].items():
        if (not re.fullmatch(r'[A-Za-z_]\w*', name) or name.startswith('ReaWeb') or name == 'ready'
                or not re.fullmatch(r'[0-9a-f]{64}', entry.get('signatureHash', ''))):
            raise ValueError(f'{name}: invalid binding identity')
        if entry.get('compatibility') not in ('compatible', 'partial'):
            raise ValueError(f'{name}: invalid compatibility')
        limitations = entry.get('limitations')
        if not isinstance(limitations, list) or any(not isinstance(x, str) or not x for x in limitations):
            raise ValueError(f'{name}: limitations must be a list of nonempty strings')
        if (entry['compatibility'] == 'partial') != bool(limitations):
            raise ValueError(f'{name}: partial bindings require documented limitations')
        js_type_names(entry.get('returns'))
        if not isinstance(entry.get('parameters'), list):
            raise ValueError(f'{name}: invalid JavaScript contract')
        optional = False
        names = set()
        for param in entry['parameters']:
            if not re.fullmatch(r'[A-Za-z_]\w*', param.get('name', '')) or param['name'] in names:
                raise ValueError(f'{name}: invalid JavaScript parameter')
            js_type_names(param.get('type'))
            names.add(param['name'])
            if type(param.get('optional')) is not bool or (optional and not param['optional']):
                raise ValueError(f'{name}: optional parameters must be trailing')
            optional = param['optional']


def coverage(schema, manifest):
    functions, bindings = schema['functions'], manifest['functions']
    reviewed = sorted(n for n in bindings if n in functions and bindings[n]['signatureHash'] == functions[n]['signatureHash'])
    needs_review = sorted(set(bindings) - set(reviewed))
    compatible = [n for n in reviewed if bindings[n]['compatibility'] == 'compatible']
    partial = [n for n in reviewed if bindings[n]['compatibility'] == 'partial']
    missing = sorted(set(functions) - set(reviewed))
    categories = {c: {'official': 0, 'implemented': 0} for c in CATEGORIES}
    for n, f in functions.items():
        categories[f['category']]['official'] += 1
        if n in reviewed:
            categories[f['category']]['implemented'] += 1
    return {'official': len(functions), 'implemented': len(reviewed), 'compatible': len(compatible),
            'partial': len(partial), 'missing': len(missing), 'implementedNames': reviewed,
            'compatibleNames': compatible, 'partialNames': partial, 'missingNames': missing,
            'needsReview': needs_review, 'categories': categories}


def compare(old, new):
    before, after = old.get('functions', {}), new['functions']
    changes = {}
    for name in sorted(before.keys() & after.keys()):
        fields = [key for key in ('parameters', 'returns', 'native', 'documentationHash', 'category')
                  if before[name].get(key) != after[name].get(key)]
        if fields:
            changes[name] = {'fields': fields, 'before': before[name]['signatures'], 'after': after[name]['signatures']}
    return {'added': sorted(after.keys() - before.keys()), 'removed': sorted(before.keys() - after.keys()),
            'changed': changes, 'sourceChanged': old.get('source') != new['source'],
            'builtinsChanged': old.get('excludedBuiltins', []) != new['excludedBuiltins']}
