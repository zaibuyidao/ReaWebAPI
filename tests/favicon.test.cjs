const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const { webcrypto } = require('node:crypto');
const script = '(() => {' + fs.readFileSync(path.join(__dirname, '../runtime/reaper-api.generated.js'), 'utf8') +
  fs.readFileSync(path.join(__dirname, '../runtime/reaper.js'), 'utf8') + '})();';
const methods = Object.keys(JSON.parse(fs.readFileSync(path.join(__dirname, '../api/bindings.json'), 'utf8')).functions);
const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><rect width="16" height="16"/></svg>';
const link = (href, options = {}) => ({ nodeType: 1, tagName: 'LINK', rel: 'icon', type: '', media: '', href,
  getAttribute(name) { return this[name]; }, querySelector() { return null; }, ...options });
const delay = () => new Promise(resolve => setTimeout(resolve, 5));
const until = async predicate => {
  for (let i = 0; i < 200; ++i) { if (predicate()) return; await delay(); }
  assert.fail('Timed out waiting for favicon synchronization');
};
function fixture(t, engine, links = [link('logo.svg')], options = {}) {
  const messages = [], requests = [], warnings = [], listeners = {}, queries = [];
  let observer, stopped = false;
  const document = { baseURI: 'http://127.0.0.1:3210/app/index.html', querySelectorAll() {}, head: { querySelectorAll: () => links } };
  const window = { document, URL, AbortController,
    addEventListener(name, callback) { listeners[name] = callback; },
    MutationObserver: class {
      constructor(callback) { observer = callback; }
      observe() {}
      disconnect() { stopped = true; }
    },
    matchMedia(media) {
      const query = { media, matches: options.media?.[media] ?? false,
        addEventListener(name, callback) { this.listener = callback; }, removeEventListener() { this.listener = null; } };
      queries.push(query); return query;
    },
    fetch(url, init) { requests.push({ url, signal: init.signal }); return options.fetch ? options.fetch(url, init) : Promise.resolve(new Response(svg)); }
  };
  window.top = window;
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
  vm.runInNewContext(script, { window, crypto: webcrypto, TextEncoder, Uint8Array, Float64Array, DataView, btoa, atob, DOMException,
    setTimeout, clearTimeout, console: { warn: (...values) => warnings.push(values), error: (...values) => warnings.push(values) } });
  t.after(() => listeners.pagehide());
  const commits = () => messages.filter(m => m.method === 'ReaWeb_Favicon' && Object.hasOwn(m.args[0], 'icon'));
  return { window, document, links, messages, requests, warnings, queries, commits, listeners, stopped: () => stopped,
    mutate() { if (!stopped) observer([{ type: 'attributes', target: { tagName: 'LINK' } }]); } };
}
for (const engine of ['windows', 'webkit']) {
  test(`${engine}: declared favicon resolves against the document URL and follows replacement/removal`, async t => {
    const f = fixture(t, engine, [link('logo.svg?v=1#icon', { type: 'image/svg+xml', rel: 'shortcut ICON' })]);
    await until(() => f.commits().length === 1);
    assert.equal(f.requests[0].url, 'http://127.0.0.1:3210/app/logo.svg?v=1#icon');
    assert.equal(Buffer.from(f.commits()[0].args[0].icon.bytes.__reawebBytes, 'base64').toString(), svg);
    f.document.baseURI = 'http://127.0.0.1:3210/assets/'; f.links[0].href = '图标.svg'; f.mutate();
    await until(() => f.commits().length === 2);
    assert.equal(f.requests[1].url, 'http://127.0.0.1:3210/assets/%E5%9B%BE%E6%A0%87.svg');
    f.links.length = 0; f.mutate();
    await until(() => f.commits().length === 3);
    assert.equal(f.commits()[2].args[0].icon, null);
    assert.equal(f.requests.length, 2);
    assert.equal(f.warnings.length, 0);
  });
}
test('new favicon invalidates a delayed fetch and unload disconnects observation', async t => {
  const fetches = [];
  const f = fixture(t, 'windows', [link('old.svg')], { fetch: () => new Promise(resolve => fetches.push(resolve)) });
  await until(() => fetches.length === 1);
  f.links[0].href = 'new.svg'; f.mutate();
  await until(() => fetches.length === 2);
  assert.ok(f.requests[0].signal.aborted);
  fetches[1](new Response(svg));
  await until(() => f.commits().length === 1);
  fetches[0](new Response(svg)); await delay();
  assert.equal(f.commits()[0].args[0].revision, 2);
  assert.equal(f.commits().length, 1);
  f.listeners.pagehide(); assert.ok(f.stopped());
  assert.ok(f.requests[1].signal.aborted);
});
test('explicit native override stops automatic synchronization without fetching', async t => {
  const f = fixture(t, 'webkit', undefined, { reply: () => ({ result: false }) });
  await until(f.stopped);
  assert.equal(f.requests.length, 0);
  assert.equal(f.commits().length, 0);
});
test('last supported declaration follows media changes and ignores unrelated mutations', async t => {
  const media = { '(prefers-color-scheme: dark)': false };
  const f = fixture(t, 'windows', [link('light.png'), link('dark.svg', { media: '(prefers-color-scheme: dark)' }), link('unsupported.gif')], { media });
  await until(() => f.commits().length === 1);
  assert.ok(f.requests[0].url.endsWith('light.png'));
  f.mutate(); await delay(); assert.equal(f.requests.length, 1);
  media['(prefers-color-scheme: dark)'] = true;
  f.queries.findLast(query => query.listener).listener();
  await until(() => f.commits().length === 2);
  assert.ok(f.requests[1].url.endsWith('dark.svg'));
});
test('data and blob favicons use their declared image formats', async t => {
  const f = fixture(t, 'windows', [link('data:image/svg+xml,' + encodeURIComponent(svg))]);
  await until(() => f.commits().length === 1);
  assert.equal(f.commits()[0].args[0].icon.format, '.svg');
  f.links[0].href = 'blob:http://127.0.0.1:3210/id'; f.links[0].type = 'image/x-icon'; f.mutate();
  await until(() => f.commits().length === 2);
  assert.equal(f.commits()[1].args[0].icon.format, '.ico');
});
test('failed and oversized fetches preserve the native icon and allow later changes', async t => {
  let response = () => new Response('missing', { status: 404 });
  const f = fixture(t, 'windows', undefined, { fetch: () => Promise.resolve(response()) });
  await until(() => f.warnings.length === 1);
  assert.equal(f.commits().length, 0);
  assert.ok(f.requests[0].signal.aborted);
  response = () => new Response(new Uint8Array(4 * 1024 * 1024 + 1));
  f.links[0].href = 'large.png'; f.mutate();
  await until(() => f.warnings.length === 2);
  assert.equal(f.commits().length, 0);
  assert.ok(f.requests[1].signal.aborted);
  response = () => new Response(svg); f.links[0].href = 'valid.svg'; f.mutate();
  await until(() => f.commits().length === 1);
});
test('a page without a declared favicon clears the previous document icon without guessing a URL', async t => {
  const f = fixture(t, 'webkit', []);
  await until(() => f.commits().length === 1);
  assert.equal(f.commits()[0].args[0].icon, null);
  assert.equal(f.requests.length, 0);
});
