const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const { webcrypto } = require('node:crypto');
const script = '(() => {' + fs.readFileSync(path.join(__dirname, '../runtime/reaper-api.generated.js'), 'utf8') +
  fs.readFileSync(path.join(__dirname, '../runtime/reaper.js'), 'utf8') + '})();';
const methods = Object.keys(JSON.parse(fs.readFileSync(path.join(__dirname, '../api/bindings.json'), 'utf8')).functions);
const delay = () => new Promise(resolve => setTimeout(resolve, 5));
const until = async predicate => {
  for (let i = 0; i < 200; ++i) { if (predicate()) return; await delay(); }
  assert.fail('Timed out waiting for window title synchronization');
};
function fixture(t, engine, initial = '', options = {}) {
  const messages = [], warnings = [], listeners = {};
  let observer, stopped = false;
  const document = { title: initial };
  const window = { document,
    addEventListener(name, callback) { listeners[name] = callback; },
    MutationObserver: class {
      constructor(callback) { observer = callback; }
      observe(target, config) {
        assert.equal(target, document);
        assert.deepEqual(JSON.parse(JSON.stringify(config)), { subtree: true, childList: true, characterData: true });
      }
      disconnect() { stopped = true; }
    }
  };
  window.top = options.iframe ? {} : window;
  const post = text => {
    const message = JSON.parse(text); messages.push(message);
    queueMicrotask(() => {
      const payload = message.method === '__reawebHello' ? { result: { protocol: 1, projectEpoch: 1, methods } }
        : options.reply?.(message) || { result: true };
      window.__reawebReceive({ id: message.id, document: message.document, ...payload });
    });
  };
  if (engine === 'windows') window.chrome = { webview: { postMessage: post } };
  else window.webkit = { messageHandlers: { reaweb: { postMessage: post } } };
  vm.runInNewContext(script, { window, crypto: webcrypto, TextEncoder, Uint8Array, Float64Array, DataView, btoa, atob,
    setTimeout, clearTimeout, console: { warn: (...values) => warnings.push(values), error: (...values) => warnings.push(values) } });
  t.after(() => listeners.pagehide?.());
  return { window, document, messages, warnings, listeners, stopped: () => stopped,
    titles: () => messages.filter(m => m.method === 'ReaWeb_DocumentTitle').map(m => m.args[0]),
    mutate(records = [{ type: 'childList', target: { tagName: 'TITLE' }, addedNodes: [], removedNodes: [] }]) {
      if (!stopped) observer(records);
    } };
}
for (const engine of ['windows', 'webkit']) {
  test(`${engine}: HTML title and document.title changes sync without explicit overrides`, async t => {
    const f = fixture(t, engine, 'SendFlow');
    await until(() => f.titles().length === 1);
    assert.deepEqual(f.titles(), ['SendFlow']);
    f.document.title = '我的工具 🎵'; f.mutate();
    await until(() => f.titles().length === 2);
    assert.equal(f.titles()[1], '我的工具 🎵');
    f.mutate(); await delay(); assert.equal(f.titles().length, 2);
    f.document.title = ''; f.mutate();
    await until(() => f.titles().length === 3);
    assert.equal(f.titles()[2], '');
    assert.ok(!f.messages.some(m => m.method === 'ReaWeb_SetTitle'));
    assert.deepEqual(f.warnings, []);
  });
}
test('late title creation, text edits and head replacement/removal are observed', async t => {
  const f = fixture(t, 'webkit');
  await until(() => f.titles().length === 1);
  f.document.title = 'Late title';
  f.mutate([{ type: 'childList', target: {}, addedNodes: [{ nodeType: 1, tagName: 'TITLE' }], removedNodes: [] }]);
  await until(() => f.titles().length === 2);
  f.document.title = 'Text edit';
  f.mutate([{ type: 'characterData', target: { parentNode: { tagName: 'TITLE' } } }]);
  await until(() => f.titles().length === 3);
  f.document.title = 'Replaced head';
  f.mutate([{ type: 'childList', target: {}, addedNodes: [{ nodeType: 1, tagName: 'HEAD' }], removedNodes: [] }]);
  await until(() => f.titles().length === 4);
  f.document.title = '';
  f.mutate([{ type: 'childList', target: {}, addedNodes: [], removedNodes: [{ nodeType: 1, tagName: 'HEAD' }] }]);
  await until(() => f.titles().length === 5);
  assert.deepEqual(f.titles(), ['', 'Late title', 'Text edit', 'Replaced head', '']);
});
test('whitespace and NUL normalize, rapid mutations coalesce, unload cancels updates', async t => {
  const f = fixture(t, 'windows', '  \t\n');
  await until(() => f.titles().length === 1);
  assert.deepEqual(f.titles(), ['']);
  f.document.title = 'Intermediate'; f.mutate();
  f.document.title = '  Rea\0GBA  '; f.mutate();
  await until(() => f.titles().length === 2);
  assert.deepEqual(f.titles(), ['', 'ReaGBA']);
  f.document.title = 'After unload'; f.mutate(); f.listeners.pagehide();
  await delay(); assert.ok(f.stopped()); assert.equal(f.titles().length, 2);
});
test('native explicit override stops automatic observation', async t => {
  const f = fixture(t, 'windows', 'HTML', { reply: () => ({ result: false }) });
  await until(f.stopped);
  f.document.title = 'Ignored'; f.mutate(); await delay();
  assert.deepEqual(f.titles(), ['HTML']);
});
test('failed synchronization reports a warning and later title changes still sync', async t => {
  let fail = true;
  const f = fixture(t, 'windows', 'First', { reply: () => fail ? { error: { code: 'QUEUE_LIMIT', message: 'Busy' } } : { result: true } });
  await until(() => f.warnings.length === 1);
  fail = false; f.document.title = 'Recovered'; f.mutate();
  await until(() => f.titles().length === 2);
  assert.deepEqual(f.titles(), ['First', 'Recovered']);
});
test('subframe titles cannot control the host window', async t => {
  const f = fixture(t, 'webkit', 'Iframe', { iframe: true });
  await delay(); assert.deepEqual(f.messages, []); assert.equal(f.window.reaper, undefined);
});
