import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import vm from 'node:vm';

const require = createRequire(resolve('examples/templates/modern/package.json'));
const load = name => import(pathToFileURL(require.resolve('typescript/unstable/' + name)));
const { API } = await load('sync');
const { isInterfaceDeclaration, isTypeAliasDeclaration, isVariableStatement, SyntaxKind } = await load('ast');
const isMethodSignature = node => node.kind === SyntaxKind.MethodSignature;
const clean = text => text.replaceAll('\r\n', '\n').replace(/\/\*\*[\s\S]*?\*\/\s*/g, '').trim();
const source = node => clean(node.getText());
function comment(node) {
  const trivia = node.getFullText().slice(0, node.getStart() - node.pos);
  return [...trivia.matchAll(/\/\*\*([\s\S]*?)\*\//g)].at(-1)?.[1]
    .replace(/\s*\n\s*\*?\s*/g, ' ').trim() || '';
}

function signatures(nodes, prefix = '') {
  return nodes.map(node => ({
    text: prefix + source(node).replace(/;$/, '').replace(/^readonly /, ''),
    parameters: (node.parameters || []).map(p => ({name: source(p.name), type: source(p.type),
      optional: Boolean(p.questionToken || p.initializer || p.dotDotDotToken)})),
    result: source(node.type), note: comment(node)
  }));
}

function groups(nodes) {
  const result = new Map();
  for (const node of nodes) {
    const name = source(node.name).replace(/^['"]|['"]$/g, '');
    if (!result.has(name)) result.set(name, []);
    result.get(name).push(node);
  }
  return result;
}

function contract(project) {
  const files = ['runtime/reaper.d.ts', 'runtime/runtime-api.d.ts'].map(path => {
    const filename = resolve(path);
    assert.equal(project.program.getSyntacticDiagnostics(filename).length, 0, path);
    assert.equal(project.program.getSemanticDiagnostics(filename).length, 0, path);
    return project.program.getSourceFile(filename);
  });
  const statements = files.flatMap(f => [...f.statements]);
  const global = statements.filter(isVariableStatement).flatMap(s => [...s.declarationList.declarations])
    .find(d => source(d.name) === 'reaper');
  const checker = project.checker;
  const actual = checker.getTypeAtLocation(global.name);
  const publicNames = checker.getPropertiesOfType(actual).map(s => s.name).sort();
  const types = groups(statements.filter(n => (isInterfaceDeclaration(n) || isTypeAliasDeclaration(n))
    && source(n.name).startsWith('ReaWeb')));
  const api = types.get('ReaWebAPI').flatMap(n => [...n.members]);
  const entries = [];
  const window = { addEventListener() {} };
  window.top = window;
  vm.runInNewContext(readFileSync('runtime/reaper.js', 'utf8'), {
    window, crypto, TextEncoder, Uint8Array, Float64Array, DataView, btoa, atob, setTimeout, clearTimeout, console
  });
  assert.deepEqual(publicNames, Object.keys(window.reaper).sort(), 'Global namespaces differ from runtime/reaper.js');
  for (const [ns, declarations] of groups(api)) {
    const members = groups(declarations.flatMap(n => [...n.type.members]));
    assert.deepEqual([...members.keys()].sort(), Object.keys(window.reaper[ns]).sort(), ns);
    for (const [name, nodes] of members) {
      const method = isMethodSignature(nodes[0]);
      assert.equal(typeof window.reaper[ns][name] === 'function', method, ns + '.' + name);
      entries.push({ id: 'runtime.' + ns + '.' + name, name: 'reaper.' + ns + '.' + name,
        group: 'reaper.' + ns, kind: method ? 'method' : 'property', signatures: signatures(nodes, 'reaper.' + ns + '.') });
    }
  }
  for (const [name, nodes] of types) {
    if (name === 'ReaWebAPI') continue;
    if (name === 'ReaWebEvents') {
      for (const [event, members] of groups(nodes.flatMap(n => [...n.members]))) {
        entries.push({ id: 'event.' + event, name: event, group: 'events', kind: 'event', signatures: signatures(members) });
      }
    }
    entries.push({ id: 'type.' + name, name, group: 'types', kind: 'type',
      definitions: nodes.map(node => node.getText().replaceAll('\r\n', '\n')), note: nodes.map(comment).filter(Boolean).join('\n') });
    if (nodes.some(n => n.members?.some(isMethodSignature))) {
      for (const [member, declarations] of groups(nodes.flatMap(n => [...(n.members || [])]))) {
        entries.push({id: 'type.' + name + '.' + member, name: name + '.' + member, group: 'members',
          kind: isMethodSignature(declarations[0]) ? 'method' : 'property', signatures: signatures(declarations)});
      }
    }
  }
  return entries;
}

const api = new API();
try {
  const config = resolve('tools/api-reference/tsconfig.json');
  const snapshot = api.updateSnapshot({openProjects: [config]});
  process.stdout.write(JSON.stringify(contract(snapshot.getProject(config))));
} finally {
  api.close();
}
