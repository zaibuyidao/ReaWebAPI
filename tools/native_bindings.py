"""Build-time native ABI compiler. API discovery stays in tools.api_sync.

Every C parameter and every Lua input/output must have exactly one mapping.
Unknown conventions fail the build instead of producing a stub.
"""
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HANDLES = {'MediaTrack', 'MediaItem', 'MediaItem_Take', 'TrackEnvelope', 'ReaProject',
           'PCM_source', 'AudioAccessor', 'HWND', 'KbdSectionInfo', 'joystick_device',
           'IReaperControlSurface', 'ProjectMarker'}
SCALARS = {'int': 'Int', 'unsigned int': 'UInt', 'size_t': 'Size', 'bool': 'Bool', 'double': 'Double'}
NULLABLE_HANDLES = {'ReaProject', 'HWND', 'IReaperControlSurface', 'KbdSectionInfo', 'void'}
NULLABLE_CALLS = {'SetOnlyTrackSelected', 'SetCursorContext', 'GetTakeName'}
# These strings contain bytes, not UTF-8 text. Preserve NUL and bytes >= 128.
BINARY = {
    'MIDI_GetAllEvts': {'buf'}, 'MIDI_SetAllEvts': {'buf'},
    'MIDI_GetEvt': {'msg'}, 'MIDI_InsertEvt': {'bytestr'}, 'MIDI_SetEvt': {'msg'},
    'MIDI_GetTextSysexEvt': {'msg'}, 'MIDI_InsertTextSysexEvt': {'bytestr'},
    'MIDI_SetTextSysexEvt': {'msg'}, 'MIDI_GetRecentInputEvent': {'buf'},
    'PCM_Sink_GetExtension': {'data'}, 'PCM_Sink_ShowConfig': {'cfg'},
    'SendMIDIMessageToHardware': {'msg'},
}


def canonical(name):
    name = re.sub(r'(?:_sz|Optional)$', '', name)
    return re.sub(r'(?:Out|InOut|Want|NeedBig|Need\d+|GUID)', '', name)


def plan(name, fn):
    native = fn['native']['parameters']
    inputs, outputs = fn['parameters'], fn['returns']
    used_in, used_out, params = set(), set(), []
    ret = fn['native']['returnType']
    if ret != 'void':
        assert outputs, name + ': missing native return'
        expected = {'int':'integer', 'unsigned int':'integer', 'double':'number', 'bool':'boolean',
                    'const char*':'string', 'GUID*':'string'}.get(ret, ret.rstrip('*'))
        if outputs[0]['type'] != expected:
            raise ValueError(name + ': native/Lua return type mismatch')
        used_out.add(0)
    def find(entries, used, key):
        found = [i for i, p in enumerate(entries) if i not in used and canonical(p['name']) == canonical(key)]
        if len(found) > 1:
            raise ValueError(name + ': ambiguous parameter ' + key)
        if found:
            used.add(found[0])
            return found[0]
        return -1
    for index, p in enumerate(native):
        typ, key = p['type'], p['name']
        item = dict(type=typ, name=key, input=-1, output=-1, link=-1, flags=0, handle='')
        # Native string lengths disappear from Lua. Their lifetime follows the buffer.
        if key.endswith('_sz'):
            base = key[:-3]
            match = [i for i, v in enumerate(native) if v['name'] == base]
            if len(match) != 1 or native[match[0]]['type'] not in ('char*', 'const char*'):
                raise ValueError(name + ': unknown buffer length ' + key)
            item.update(kind='LengthPtr' if typ == 'int*' else 'Length', link=match[0])
        elif typ in ('RECT*', 'const RECT*'):
            base = key.replace('InOut', '')
            ins = [find(inputs, used_in, base + '.' + c) for c in ('left', 'top', 'right', 'bot')]
            outs = [find(outputs, used_out, base + '.' + c) for c in ('left', 'top', 'right', 'bot')]
            if any(i < 0 for i in ins) or ins != list(range(ins[0], ins[0] + 4)):
                raise ValueError(name + ': invalid RECT input')
            if outs != [-1] * 4 and outs != list(range(outs[0], outs[0] + 4)):
                raise ValueError(name + ': invalid RECT output')
            item.update(kind='Rect', input=ins[0], output=outs[0])
        else:
            item['input'] = find(inputs, used_in, key)
            item['output'] = find(outputs, used_out, key)
            base = typ.replace('const ', '').rstrip('*')
            if typ in SCALARS:
                item['kind'] = SCALARS[typ]
            elif typ in ('char*', 'const char*', 'const char**'):
                item['kind'] = {'char*': 'Buffer', 'const char*': 'String', 'const char**': 'StringOut'}[typ]
                if 'NeedBig' in key: item['flags'] |= 1
                if canonical(key) in BINARY.get(name, set()): item['flags'] |= 2
            elif base == 'GUID':
                item['kind'] = 'Guid'
            elif base in SCALARS:
                item['kind'] = SCALARS[base] + 'Ptr'
                if item['input'] >= 0 and inputs[item['input']]['type'] == 'reaper.array':
                    item['kind'] = 'Array'
            elif base in HANDLES or base == 'void':
                item.update(kind='HandleOut' if typ.endswith('**') else 'Handle', handle=base)
                if base in NULLABLE_HANDLES or name in NULLABLE_CALLS: item['flags'] |= 8
            else:
                raise ValueError(name + ': unknown native type ' + typ)
            if item['input'] < 0 and item['output'] < 0:
                raise ValueError(name + ': unmapped native parameter ' + key)
            if item['input'] >= 0 and inputs[item['input']]['optional']: item['flags'] |= 4
        params.append(item)
    if used_in != set(range(len(inputs))) or used_out != set(range(len(outputs))):
        raise ValueError(name + ': unmapped Lua values ' + repr(([v for i,v in enumerate(inputs) if i not in used_in], [v for i,v in enumerate(outputs) if i not in used_out])))
    # Names alone are not sufficient: check ABI/Lua scalar families too.
    families = {'Int':'integer', 'UInt':'integer', 'Size':'integer', 'Bool':'boolean',
                'Double':'number', 'String':'string', 'Buffer':'string', 'StringOut':'string',
                'Guid':'string', 'Array':'reaper.array'}
    for p in params:
        kind = p['kind'].removesuffix('Ptr')
        expect = families.get(kind, p['handle'] if p['handle'] != 'void' else 'identifier')
        for key, entries in [('input', inputs), ('output', outputs)]:
            if p[key] >= 0 and p['kind'] != 'Rect' and entries[p[key]]['type'] != expect:
                raise ValueError(f'{name}.{p["name"]}: {key} type {entries[p[key]]["type"]}, expected {expect}')
    return {'name': name, 'parameters': params, 'returnType': ret, 'returnCount': len(outputs),
            'minArgs': sum(not p['optional'] for p in inputs), 'maxArgs': len(inputs)}


def plans(schema):
    result = [plan(n, f) for n, f in sorted(schema['functions'].items())]
    assert len(result) == len(schema['functions'])
    return result


def typescript_type(typ, output=False, binary=False):
    if binary: return 'Uint8Array' if output else 'Uint8Array | string'
    if typ in ('integer', 'number'): return 'number'
    if typ == 'reaper.array': return 'Float64Array | number[]'
    if typ == 'identifier': return 'ReaWebHandle | null'
    if typ in HANDLES:
        return typ + 'Handle | null' + (' | 0' if typ == 'ReaProject' and not output else '')
    return typ


def manifest(schema):
    plans(schema)  # No declarations without a complete native lowering.
    result = {'bindingVersion': 1, 'functions': {}}
    for n, f in sorted(schema['functions'].items()):
        params = [{'name': re.sub(r'\W', '_', p['name']) + ('_' if p['name'] in ('in', 'export', 'function', 'var', 'new') else ''),
                   'type': typescript_type(p['type'], binary=canonical(p['name']) in BINARY.get(n, set())),
                   'optional': p['optional']} for p in f['parameters']]
        for source, target in zip(f['parameters'], params):
            if source['type'] in HANDLES and source['type'] not in NULLABLE_HANDLES and n not in NULLABLE_CALLS and not source['optional']:
                target['type'] = source['type'] + 'Handle'
            elif source['type'] == 'KbdSectionInfo':
                target['type'] += ' | 0'
        returns = [typescript_type(p['type'], True, canonical(p['name']) in BINARY.get(n, set())) for p in f['returns']]
        if f['native']['returnType'] in ('const char*', 'GUID*'):
            returns[0] += ' | null'
        for param in plan(n, f)['parameters']:
            if param['kind'] == 'StringOut': returns[param['output']] += ' | null'
        result['functions'][n] = {'signatureHash': f['signatureHash'], 'compatibility': 'compatible',
            'parameters': params, 'returns': 'void' if not returns else returns[0] if len(returns) == 1 else '[' + ', '.join(returns) + ']', 'limitations': []}
    return result


def cpp(schema):
    rows = plans(schema)
    lines = ['// Generated in the build directory. Never edit.', '#include "core/native_call.hpp"',
             '#define REAPERAPI_FUNCNAME(name) reaweb_sdk_##name', '#include <reaper_plugin_functions.h>',
             '#undef REAPERAPI_FUNCNAME', 'namespace reaweb {', 'namespace {']
    for i, row in enumerate(rows):
        p = row['parameters']
        lines += [f'void invoke_{i}([[maybe_unused]] NativeFrame& f, void* address) {{',
                  '  using Function = ' + row['returnType'] + ' (*)(' + ', '.join(a['type'] for a in p) + ');']
        lines.append(f'  static_assert(std::is_same_v<Function, decltype(reaweb_sdk_{row["name"]})>, "SDK ABI mismatch: {row["name"]}");')
        call = 'reinterpret_cast<Function>(address)(' + ', '.join(f'f.argument<{a["type"]}>({j})' for j,a in enumerate(p)) + ')'
        lines.append('  ' + (call + ';' if row['returnType'] == 'void' else 'f.capture(' + call + ');'))
        lines += ['}']
    lines += ['}', 'const std::vector<NativeEntry>& native_entries() {', '  static const std::vector<NativeEntry> entries = {']
    for i, row in enumerate(rows):
        specs = ', '.join('{Kind::%s, %d, %d, %d, %d, "%s"}' % (p['kind'], p['input'], p['output'], p['link'], p['flags'], p['handle']) for p in row['parameters'])
        lines.append('    {"%s", "%s", %d, %d, %d, {%s}, invoke_%d},' % (row['name'], row['returnType'], row['minArgs'], row['maxArgs'], row['returnCount'], specs, i))
    lines += ['  };', '  return entries;', '}', '}']
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--manifest', action='store_true')
    args = parser.parse_args()
    schema = json.loads((ROOT / 'api/reaper_api.json').read_text(encoding='utf-8-sig'))
    if args.manifest:
        (ROOT / 'api/bindings.json').write_text(json.dumps(manifest(schema), ensure_ascii=False, indent=2) + '\n', encoding='utf-8', newline='\n')
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(cpp(schema), encoding='utf-8', newline='\n')
        script = '(() => {\n' + (ROOT / 'runtime/reaper-api.generated.js').read_text(encoding='utf-8') + '\n' + (ROOT / 'runtime/reaper.js').read_text(encoding='utf-8-sig') + '\n})();'
        chunks = '\n'.join('  value += ' + json.dumps(script[i:i+2000], ensure_ascii=True) + ';' for i in range(0, len(script), 2000))
        (args.output.parent / 'bridge_script.hpp').write_text('#pragma once\n#include <string>\nnamespace reaweb {\ninline const std::string bridge_script = [] {\n  std::string value;\n' + chunks + '\n  return value;\n}();\n}\n', encoding='utf-8', newline='\n')
    print(f'Validated complete native mappings: {len(plans(schema))}/{len(schema["functions"])}')


if __name__ == '__main__':
    main()
