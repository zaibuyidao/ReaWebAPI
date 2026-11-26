"""Generate and verify the offline API reference used by all packages."""
import argparse
import hashlib
from html import escape
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

if not __package__:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.version import ROOT, source_version

NAME = 'reawebapihelp.html'
INSTALLED = 'ReaWebAPI/docs/' + NAME
TEMPLATES = ROOT / 'tools/api-reference'
DOCS = ('runtime-api-inventory', 'host-api', 'host-api.zh-CN', 'eel-backend-compatibility',
        'runtime-api', 'runtime-api.zh-CN', 'native-services', 'native-services.zh-CN',
        'file-export', 'file-export.zh-CN', 'realtime', 'realtime.zh-CN', 'eel-backend-compatibility.zh-CN',
        'jsfx-webui', 'jsfx-webui.zh-CN', 'development')
DATA_MARK = '<script type="application/json" id="reference-data">'


def revision_id():
    if os.environ.get('GITHUB_SHA'):
        return os.environ['GITHUB_SHA']
    if shutil.which('git'):
        result = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, capture_output=True, text=True)
        if result.returncode == 0:
            return result.stdout.strip()
    return 'local'


def rows(document):
    for line in document.splitlines():
        if line.startswith('|') and line.endswith('|'):
            yield [cell.strip().replace(r'\|', '|') for cell in re.split(r'(?<!\\)\|', line[1:-1])]


def plain(text):
    return re.sub(r'\[([^]]+)\]\([^)]+\)', r'\1', text).replace(chr(96), '')


def add_note(entry, note, language=None):
    note = plain(note).strip()
    if not note:
        return
    key = language or ('zh' if re.search(r'[\u3400-\u9fff]', note) else 'en')
    note = note.replace('；', '。') if key == 'zh' else note
    summary = entry.setdefault(key, note)
    if entry['kind'] != 'host' and note != summary:
        details = entry.setdefault('detail' + key.title(), [])
        if note not in details:
            details.append(note)


def hosts():
    source = (ROOT / 'src/plugin/plugin.cpp').read_text(encoding='utf-8')
    registered = re.findall(r'^\s*add_api\("(\w+)"', source, re.M)
    definitions = re.findall(r'^\s*add_api\("(\w+)"[^;]*?,\s*"([^"]+)"\s*\);', source, re.M)
    if [name for name, _ in definitions] != registered or not registered:
        raise ValueError('Unrecognized ReaScript registration')
    lua_types = {'int': 'integer', 'bool': 'boolean', 'bool*': 'boolean', 'const char*': 'string'}
    py_types = {'int': 'Int', 'bool': 'Boolean', 'bool*': 'Boolean', 'const char*': 'String'}
    for name, definition in definitions:
        returns, inputs, names, note, _ = definition.split(r'\0')
        params = list(zip(filter(None, names.split(',')), filter(None, inputs.split(',')), strict=True))
        c_args = ', '.join(f'{typ} {key}' for key, typ in params)
        lua_args, eel_args = [], []
        for key, typ in params:
            arg = key.removesuffix('Optional')
            optional = 'optional ' if arg != key else ''
            lua_args.append(f'{optional}{lua_types[typ]} {arg}')
            eel_args.append(optional + (f'"{arg}"' if typ == 'const char*' else f'{typ.rstrip("*")} {arg}'))
        eel_result = returns
        if returns == 'const char*':
            eel_args.insert(0, '#retval')
            eel_result = 'bool'
        py_args = ', '.join(f'{py_types[typ]} {key}' for key, typ in params)
        python = f'{py_types[returns]} {name}({py_args})'
        if any(typ == 'bool*' for _, typ in params):
            python = f'({py_types[returns]} retval, {py_args}) = {name}({", ".join(key for key, _ in params)})'
        signatures = [
            ('C', f'{returns} {name}({c_args})'),
            ('EEL2', f'{eel_result} {name}({", ".join(eel_args)})'),
            ('Lua', f'{lua_types[returns]} reaper.{name}({", ".join(lua_args)})'),
            ('Python', python)
        ]
        yield dict(id='host.' + name, name=name, group='host', kind='host', en=note,
                   signatures=[dict(label=label, text=text) for label, text in signatures])


def jsfx_methods(document):
    methods = {}
    for cells in rows(document):
        if not cells[0].startswith(chr(96)):
            continue
        if len(cells) == 2:
            method = re.findall(r'\x60([\w.]+)\x60', cells[0])
            if method and method[-1] not in ('paramsChanged', 'visibilityChanged') and (
                    ' / ' in cells[0] or method[-1] in ('getCapabilities', 'present', 'getDiagnostics')):
                methods[method[-1]] = plain(cells[1])
        elif len(cells) == 3:
            match = re.match(r'\x60(\w+(?:/\w+)*)\x60', cells[1])
            if match:
                for method in match[1].split('/'):
                    methods[method] = plain(cells[2])
    return methods


def render(version, revision):
    docs = {name: (ROOT / f'docs/{name}.md').read_text(encoding='utf-8') for name in DOCS}
    entries = json.loads(subprocess.check_output(['node', str(TEMPLATES / 'contract.mjs')], cwd=ROOT))
    entries.extend(hosts())
    names = {e['name']: e for e in entries}
    for document in ('host-api', 'host-api.zh-CN', 'runtime-api-inventory', 'runtime-api', 'runtime-api.zh-CN',
                     'native-services', 'native-services.zh-CN', 'file-export', 'file-export.zh-CN'):
        for cells in rows(docs[document]):
            if len(cells) < 2:
                continue
            for token in re.findall(r'\x60([^\x60]+)\x60', cells[0]):
                key = token.partition('(')[0]
                if key.startswith('reaper.ReaWeb_'):
                    key = key.removeprefix('reaper.')
                if key.startswith('writer.'):
                    key = key.replace('writer.', 'ReaWebFileWriter.', 1)
                if key in names:
                    add_note(names[key], cells[-1])
        # Attach a paragraph only to the call it introduces. Cross-references in
        # the same paragraph can describe a different API.
        prose = re.sub(r'^\x60{3}[^\n]*\n.*?^\x60{3}[^\n]*', '', docs[document], flags=re.M | re.S)
        for paragraph in re.split(r'\n\s*\n', prose):
            match = re.match(r'\x60((?:reaper\.\w+\.\w+|ReaWeb_\w+))(?=[(\x60])', paragraph)
            if match and match[1] in names:
                entry = names[match[1]]
                key = 'detailZh' if document.endswith('.zh-CN') else 'detailEn'
                value = plain(' '.join(paragraph.splitlines())).replace('；', '。')
                notes = entry.setdefault(key, [])
                if value not in notes:
                    notes.append(value)
    for document in ('runtime-api', 'development', 'native-services', 'host-api', 'eel-backend-compatibility',
                     'realtime', 'file-export'):
        for lang, code in re.findall(r'^~~~(js|javascript|lua|eel)\s*\n(.*?)^~~~', docs[document].replace(chr(96)*3, '~~~'), re.M | re.S):
            if len(code) > 1400:
                continue
            for name in re.findall(r'\breaper\.(?:\w+\.\w+|ReaWeb_\w+)|\bReaWeb_\w+(?=\()', code):
                entry = names.get(name) or names.get(name.removeprefix('reaper.'))
                if entry:
                    entry.setdefault('examples', {}).setdefault(lang.replace('javascript', 'js'), code.strip())
    service_docs = {
        'runtime': {'getInfo': 'Extension version and service ABI.', 'getHostVersion': 'REAPER version string.',
                    'getProjectSummary': 'Project name, track count and play state.',
                    'getTransportSnapshot': 'Transport state, position, cursor and tempo. Requires an available host snapshot.'},
        'jsfx': {}
    }
    runtime = (ROOT / 'src/runtime/runtime.cpp').read_text(encoding='utf-8')
    builtins = set(re.findall(r'\boperation == "(\w+)"', runtime))
    if builtins != set(service_docs['runtime']):
        raise ValueError('Update the documented built-in runtime service methods')
    service_docs['jsfx'] = jsfx_methods(docs['jsfx-webui'])
    service_zh = {'jsfx': jsfx_methods(docs['jsfx-webui.zh-CN']), 'runtime': {
        'getInfo': '读取扩展版本和服务 ABI。', 'getHostVersion': '读取 REAPER 版本字符串。',
        'getProjectSummary': '读取工程名称、轨道数和播放状态。',
        'getTransportSnapshot': '读取走带状态、播放位置、光标和速度。需要可用的宿主快照。'}}
    for service, methods in service_docs.items():
        condition = ('Built-in runtime service.' if service == 'runtime' else
                     'Bound JSFX Hosted WebView. DSP features require support from the associated effect.')
        for method, note in methods.items():
            entry = dict(id=f'service.{service}.{method}', name=f'{service}.{method}', group='services',
                         kind='service', en=condition + ' ' + note, related=['type.ReaWebHostService'])
            prefix = '内置运行时服务。' if service == 'runtime' else '适用于已绑定的 JSFX WebView。DSP 功能需要关联效果器支持。'
            entry['zh'] = prefix + service_zh[service][method]
            if service == 'runtime':
                entry['examples'] = {'js': f"await reaper.host.service('runtime').invoke('{method}')"}
            entries.append(entry)
    host_notes = {
        'Open': '打开本地 HTML，相对路径以 REAPER 资源目录的 Scripts 为基准。成功返回窗口 ID，失败返回 0。',
        'OpenEx': '使用 JSON 指定实例键、窗口层级、初始外观和鼠标策略。复用已有窗口时保留其配置。成功返回窗口 ID，失败返回 0。',
        'OpenDev': '打开明确受信任的本机回环 HTTP 开发页面，失败返回 0。',
        'Close': '关闭指定 ID 的 ReaWebAPI 窗口。',
        'Show': '显示窗口并保留当前焦点。窗口 ID 无效或已关闭时返回 false。',
        'IsOpen': '窗口仍然打开或正在初始化时返回 true。',
        'IsReady': '当前页面与宿主完成协议握手后返回 true。',
        'Focus': '激活浮动窗口或相应的 Docker 标签页，并将焦点交给 WebView。',
        'DevTools': '请求打开原生开发者工具。当前平台无法控制检查器时返回 false。',
        'SetShortcutRouting': '传入 true 将非编辑状态下的键盘输入交给 REAPER 主区快捷键。默认关闭，返回实际启用状态。关闭成功和出错都返回 false。',
        'SetMouseMode': '将鼠标策略设为 normal、passthrough 或 regions。失败返回 false，原因可通过 ReaWeb_GetLastError 查询。',
        'SetMouseRegions': '用 JSON 矩形数组替换鼠标交互区域，坐标以视口 CSS 像素为单位。失败返回 false。',
        'SetDocked': '停靠窗口或恢复浮动，返回操作后的停靠状态。成功取消停靠时也返回 false。',
        'IsDocked': '指定窗口位于 REAPER Docker 内时返回 true。',
        'Send': '将 UTF-8 文本加入已就绪页面的消息队列。成功入队返回 true。每条消息上限 1 MiB，不能包含 NUL。',
        'Receive': '非阻塞读取下一条来自 WebView 的消息。队列为空或出错时返回空字符串。',
        'SetFrame': '替换下一帧的文本，连续写入只保留最新值。上限 1 MiB，不能包含 NUL。',
        'GetDiagnostics': '返回指定窗口的 JSON 诊断数据。发生错误时返回空 JSON 对象。',
        'GetLastError': '读取最近一次原生入口错误。异步初始化错误也会写入 REAPER 控制台。'
    }
    for suffix, description in host_notes.items():
        names['ReaWeb_' + suffix]['zh'] = description
    for label, page in [('JSFX Web UI SDK', 'jsfx-webui'), ('External Clients', 'external-clients'),
                        ('Native C/C++ SDK', 'native-services'), ('Native Streams', 'native-streams')]:
        entries.append(dict(id='guide.' + page, name=label, group='guides', kind='guide', guide=page))
    names = {e['name']: e for e in entries}
    for row in (TEMPLATES / 'descriptions.tsv').read_text(encoding='utf-8').splitlines():
        if not row or row.startswith('#'):
            continue
        name, en, zh = row.split('\t')
        names[name].update(en=en, zh=zh)
    # These handles share the same observable lifecycle and immutable metadata.
    for prefix in ('ReaWebStream', 'ReaWebVideoSurface', 'ReaWebWorkerVideoStream', 'ReaWebRealtimeSubscription'):
        fields = {
            'closed': ('True after this handle has closed.', '此句柄关闭后为 true。'),
            'error': ('The error recorded on this handle, or null when none is recorded.', '此句柄记录的错误，无错误记录时为 null。'),
            'info': ('Read-only metadata supplied when this connection opens.', '打开连接时提供的只读元数据。'),
            'on': ('Registers the callback for the specified event. The returned function removes that listener.',
                   '注册指定事件的回调。调用返回的函数可移除此监听器。')
        }
        for field, (en, zh) in fields.items():
            if entry := names.get(prefix + '.' + field):
                entry.update(en=en, zh=zh)
    ids = {e['id'] for e in entries}
    if len(ids) != len(entries):
        raise ValueError('Duplicate API anchors')
    for e in entries:
        if e['kind'] == 'type':
            e['related'] = [item['id'] for item in entries if item['id'].startswith(e['id'] + '.')]
            if e['name'] == 'ReaWebEvents':
                e['related'] += [item['id'] for item in entries if item['kind'] == 'event']
        text = json.dumps(e.get('signatures', e.get('definitions', [])))
        links = ['type.' + name for name in sorted(set(re.findall(r'\bReaWeb\w+', text)))]
        e.setdefault('related', []).extend(link for link in links if link in ids and link != e['id'])
        if e['id'] == 'runtime.host.service':
            e['related'] += [item['id'] for item in entries if item['group'] == 'services']
    entries.sort(key=lambda e: (e['group'], e['name'].casefold()))
    data = json.dumps(dict(version=version, revision=revision, entries=entries), ensure_ascii=False, separators=(',', ':'))
    template = (TEMPLATES / 'page.html').read_text(encoding='utf-8')
    return render_html(template, entries, data, version, revision)


def bilingual(en, zh):
    if en == zh:
        return en
    return f'<span class="en">{en}</span><span class="zh" lang="zh-CN">{zh}</span>'


def render_html(template, entries, data, version, revision):
    by_name = {e['name']: e['id'] for e in entries}
    by_id = {e['id']: e for e in entries}
    name_pattern = re.compile(r'(?<![\w.])(?:' + '|'.join(re.escape(n) for n in sorted(by_name, key=len, reverse=True))
                              + r')(?![\w.-])')
    uses = {}
    for entry in entries:
        for target in entry['related']:
            if target.startswith('type.') and not target.startswith(entry['id'] + '.'):
                uses.setdefault(target, []).append(entry['id'])

    comments_zh = dict(row.split('\t') for row in (TEMPLATES / 'comments.zh-CN.tsv').read_text(encoding='utf-8').splitlines()
                       if row and not row.startswith('#'))
    guide_names = {'external-clients': '外部客户端', 'native-services': '原生 C/C++ SDK',
                   'native-streams': '原生数据流', 'jsfx-webui': 'JSFX Web UI SDK'}

    def translate_comment(match):
        note = re.sub(r'\s*\n\s*\*?\s*', ' ', match[1]).strip()
        return '/** ' + comments_zh[note] + ' */'

    def display_name(entry):
        name = escape(entry['name'])
        return bilingual(name, guide_names[entry['guide']]) if entry.get('guide') else name

    def link(entry):
        return f'<a href="#{escape(entry["id"])}">{display_name(entry)}</a>'

    def linked(text, current, syntax=False):
        def literal(value):
            safe = escape(value)
            if syntax:
                safe = re.sub(r'\b(optional|integer|boolean|string|int|bool|Int|Boolean|String)\b', r'<i>\1</i>', safe)
            return safe

        parts, end = [], 0
        for m in name_pattern.finditer(text):
            target = by_name[m[0]]
            if target != current and (target.startswith(('runtime.', 'host.', 'type.'))):
                parts += [literal(text[end:m.start()]), link(by_id[target])]
                end = m.end()
        return ''.join(parts) + literal(text[end:])

    def line(label, value, ident, translated=None):
        code = linked(value, ident, syntax=label in ('EEL2', 'Lua', 'Python'))
        if translated and translated != value:
            code = bilingual(code, linked(translated, ident))
        return (f'<div class="line" data-lang="{escape(label)}"><span>{escape(label)}:</span> <code>{code}</code>'
                '<button class="copy" data-copy="code" aria-label="Copy">⧉</button></div>')

    labels = {'events': ('Events', '事件'), 'services': ('Native services', '具名服务'),
              'members': ('Object members', '返回对象成员'), 'types': ('Types', '类型'),
              'host': ('Host API', '宿主 API'), 'guides': ('SDK guides', 'SDK 指南')}
    index, body = [], []
    groups = sorted({e['group'] for e in entries}, key=lambda g: (not g.startswith('reaper.'), g))
    for group in groups:
        items = [e for e in entries if e['group'] == group]
        title = bilingual(*labels[group]) if group in labels else escape(group)
        index.append(f'<section data-group="{escape(group)}"><h3>{title}</h3><div class="columns" style="--rows4:{(len(items)+3)//4};--rows2:{(len(items)+1)//2}">'
                     + ''.join(link(e) for e in items) + '</div></section>')
        body.append(f'<section><h2>{title}</h2>')
        for e in items:
            ident = e['id']
            body.append(f'<article id="{escape(ident)}"><h3>{display_name(e)}'
                        '<button class="copy" data-copy="link">#</button></h3>')
            sigs = e.get('signatures', [])
            for sig in sigs:
                body.append(line(sig.get('label', 'TypeScript' if e['kind'] == 'event' else 'JavaScript'),
                                 sig['text'], ident))
            for definition in e.get('definitions', []):
                translated = re.sub(r'/\*\*(.*?)\*/', translate_comment, definition, flags=re.S)
                body.append(line('TypeScript', definition, ident, translated))
            comments = [n for n in [e.get('note'), *(sig.get('note') for sig in sigs)] if n]
            for lang in ('en', 'zh'):
                notes = [e.get(lang), *(comments if lang == 'en' else [comments_zh[n] for n in comments]),
                         *e.get('detail' + lang.title(), [])]
                if not any(notes):
                    raise ValueError(f'Missing {lang} description for {ident}')
                if e['kind'] == 'host' and any('#retval' in sig['text'] for sig in sigs):
                    notes.append('EEL2 writes the returned string to its first argument, #retval.' if lang == 'en' else
                                 'EEL2 将返回字符串写入第一个参数 #retval。')
                if e['name'] == 'ReaWeb_Open':
                    notes.append('The C line shows the registered signature. Native callers can pass nullptr for the optional boolean. The Python binding returns a tuple with the window ID in its first element.' if lang == 'en'
                                 else 'C 行显示注册签名，原生调用可传入 nullptr 省略可选布尔参数。Python 绑定返回元组，第一个元素是窗口 ID。')
                for note in dict.fromkeys(n.strip() for n in notes if n and n.strip()):
                    body.append(f'<p class="{lang}" lang="{lang}">{linked(note, ident)}</p>')
            if e.get('guide'):
                rev = revision if re.fullmatch('[0-9a-f]{40}', revision) else 'main'
                base = f'https://github.com/zaibuyidao/ReaWebAPI/blob/{rev}/docs/{e["guide"]}'
                body.append('<p>' + bilingual(f'<a href="{base}.md">Online guide</a>',
                                              f'<a href="{base}.zh-CN.md">在线指南</a>') + '</p>')
            for label, example in e.get('examples', {}).items():
                translated = re.sub(r'(?m)(^|\s)(// |-- )([^\n]+)', lambda m: m[1] + m[2] +
                                    (m[3] if m[3] == '{path, bytes}' else comments_zh[m[3]]), example)
                body.append('<div class="example"><small>' + bilingual('Example', '示例') + ' · ' + escape(label)
                            + '</small><pre><code>' + bilingual(escape(example), escape(translated)) + '</code></pre>'
                            + '<button class="copy" data-copy="code">⧉</button></div>')
            for targets, caption in ((uses.get(ident, []), ('Used by', '使用此类型的条目')),
                                     (e['related'], ('See also', '相关条目'))):
                if targets:
                    body.append('<p class="related">' + bilingual(*caption) + ' · ' +
                                ', '.join(link(by_id[t]) for t in dict.fromkeys(targets)) + '</p>')
            body.append('</article>')
        body.append('</section>')
    regions = {'INDEX': ''.join(index), 'BODY': '\n'.join(body), 'VERSION': escape(version),
               'REVISION': escape(revision), 'DATA': data.replace('<', r'\u003c')}
    return re.sub(r'@@(\w+)@@', lambda m: regions[m[1]], template).encode('utf-8')


def checked(directory, version, revision):
    page = (directory / NAME).read_bytes()
    manifest = json.loads((directory / 'reference.json').read_bytes())
    if manifest != dict(version=version, revision=revision, sha256=hashlib.sha256(page).hexdigest()):
        raise ValueError('API reference version, revision or digest mismatch')
    validate_page(page, version, revision)
    return page


def validate_page(page, version, revision):
    start = page.index(DATA_MARK.encode()) + len(DATA_MARK)
    data = json.loads(page[start:page.index(b'</script>', start)])
    if (data['version'], data['revision']) != (version, revision) or not data['entries']:
        raise ValueError('Incorrect embedded API reference metadata')


def write(path, content):
    if path.exists() and path.read_bytes() == content:
        return
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as tmp:
        tmp.write(content)
    Path(tmp.name).replace(path)


def build(directory, version, revision):
    inputs = [Path(__file__), ROOT / 'tools/version.py', ROOT / 'CMakeLists.txt', ROOT / 'src/plugin/plugin.cpp', ROOT / 'src/runtime/runtime.cpp',
              ROOT / 'examples/templates/modern/package-lock.json', ROOT / 'runtime/reaper.js',
              ROOT / 'runtime/reaper.d.ts', ROOT / 'runtime/runtime-api.d.ts',
              *(TEMPLATES / name for name in ('page.html', 'contract.mjs', 'tsconfig.json', 'descriptions.tsv', 'comments.zh-CN.tsv')), *(ROOT / f'docs/{name}.md' for name in DOCS)]
    key = hashlib.sha256(json.dumps([version, revision]).encode())
    for path in inputs:
        if path.is_file():
            key.update(hashlib.sha256(path.read_bytes()).digest())
    stamp = directory / 'inputs.sha256'
    if stamp.exists() and stamp.read_text() == key.hexdigest():
        try:
            return checked(directory, version, revision)
        except (OSError, ValueError):
            pass
    page = render(version, revision)
    directory.mkdir(parents=True, exist_ok=True)
    write(directory / NAME, page)
    manifest = dict(version=version, revision=revision, sha256=hashlib.sha256(page).hexdigest())
    write(directory / 'reference.json', (json.dumps(manifest) + '\n').encode())
    write(stamp, key.hexdigest().encode())
    return page


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--revision', default=revision_id())
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    (checked if args.verify else build)(args.output, source_version(), args.revision)
