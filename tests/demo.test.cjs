const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const flush = () => new Promise(resolve => setImmediate(resolve));
const html = fs.readFileSync(path.join(__dirname, '../web/index.html'), 'utf8');
const state = (track = {}, overrides = {}) => ({
  epoch: 1, selection: 1, changeCount: 0, count: 2, version: '7.0',
  track: track === false ? false : { id: 'A', name: 'Track A', volume: 1, pan: 0, color: 0, hex: '#b7f58a', ...track },
  ...overrides
});
const project = { name: 'Test project', cursor: 0, position: 0, playState: 0, tempo: 120,
  bar: 0, beat: 0, beatsPerBar: 4, denominator: 4, total: 0, markers: 0, regions: 0, rows: [] };

async function fixture(connect = true) {
  const elements = new Map(), requests = [], copies = [], consoleLogs = [], timers = new Map(), listeners = {};
  let timerId = 0, receive, session, unsubscribed = false;
  const node = () => ({
    value: '', disabled: false, checked: false, hidden: false, listeners: {}, children: [], attributes: new Map(),
    scrollHeight: 100, scrollTop: 0, clientHeight: 100, text: '',
    get textContent() { return this.text + this.children.map(child => child.textContent).join('\n'); },
    set textContent(value) { this.text = value; this.children = []; },
    get firstElementChild() { return this.children[0]; },
    append(child) { this.children.push(child); child.remove = () => { this.children.splice(this.children.indexOf(child), 1); }; },
    replaceChildren(...children) { this.text = ''; this.children = []; children.forEach(child => this.append(child)); },
    addEventListener(name, fn) { this.listeners[name] = fn; },
    setAttribute(name, value) { this.attributes.set(name, value); },
    removeAttribute(name) { this.attributes.delete(name); },
    hasAttribute(name) { return this.attributes.has(name); },
    classList: { add() {}, remove() {} }
  });
  // Use actual page IDs so removed or renamed controls cannot be invented by the mock.
  for (const [tag, id] of html.matchAll(/<[^>]+\bid="([^"]+)"[^>]*>/g)) {
    const entry = node(); entry.id = id;
    for (const key of ['value', 'min', 'max']) entry[key] = tag.match(new RegExp(`\\b${key}="([^"]*)"`))?.[1] ?? '';
    for (const key of ['hidden', 'disabled', 'checked']) entry[key] = new RegExp(`\\s${key}(?:\\s|>)`).test(tag);
    elements.set(id, entry);
  }
  const element = id => elements.get(id);
  const document = { getElementById: element, createElement: node, activeElement: null,
    querySelectorAll(selector) { assert.equal(selector, '[id]'); return [...elements.values()]; } };
  // The demo now uses Lua messages, not direct REAPER API calls.
  const reaper = {
    events: { on: async (name, handler) => {
      assert.equal(name, 'message'); receive = handler;
      return () => { unsubscribed = true; };
    } },
    host: { send: text => {
      assert.equal(typeof receive, 'function', 'subscribe before sending ready');
      const [client, id, command, epoch, selection, ...value] = text.split('\t');
      session = client;
      requests.push({ session: client, id: Number(id), command, epoch: Number(epoch), selection: Number(selection), value: value.join('\t') });
      return true;
    } }
  };
  const context = vm.createContext({ document, window: { reaper, addEventListener: (name, fn) => { listeners[name] = fn; } },
    navigator: { clipboard: { writeText: async text => { copies.push(text); } } },
    console: Object.fromEntries(['log', 'warn', 'error'].map(level => [level, (...values) => consoleLogs.push({ level, values })])),
    setTimeout: fn => { timers.set(++timerId, fn); return timerId; }, clearTimeout: id => timers.delete(id) });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8'), context);
  await flush();
  const reply = (request, payload = {}) => receive(JSON.stringify({ type: 'response', session: request.session,
    id: request.id, ok: true, ...payload }));
  const sendState = (value, client = session) => receive(JSON.stringify({ type: 'state', session: client, state: value }));
  if (connect) {
    const ready = requests.shift(); assert.equal(ready.command, 'ready');
    reply(ready, { state: state(), data: { project } }); await flush();
    assert.equal(element('status').textContent, 'Lua connected');
    assert.equal(element('error').hidden, true);
    element('clear-log').listeners.click(); consoleLogs.length = 0;
  }
  return { context, element, document, reaper, requests, reply, sendState, copies, consoleLogs, timers, listeners,
    get unsubscribed() { return unsubscribed; } };
}

test('startup subscribes before ready and waits for Lua confirmation before enabling controls', async () => {
  const f = await fixture(false);
  assert.equal(f.requests.length, 1);
  const ready = f.requests.shift();
  assert.equal(ready.command, 'ready');
  assert.equal(ready.epoch, 0); assert.equal(ready.selection, 0);
  assert.equal(f.element('pan').disabled, true);
  assert.match(html, /<link rel="icon" type="image\/svg\+xml" href="logo\.svg" sizes="any">/);
  assert.equal([...html.matchAll(/<link\s+rel="icon"/g)].length, 1);
  f.reply(ready, { state: state({ pan: 0.25 }), data: { project } }); await flush();
  assert.equal(f.element('status').textContent, 'Lua connected');
  assert.equal(f.element('track-name').textContent, 'Track A');
  assert.equal(f.element('pan-value').textContent, '0.25');
  assert.equal(f.element('pan').disabled, false);
  assert.equal(f.element('project-name').textContent, 'Test project');
  assert.equal(f.timers.size, 0);
});

test('Lua snapshots render track values, ignore other sessions and handle deselection', async () => {
  const f = await fixture();
  f.sendState(state({ id: 'B', name: 'Track B', pan: -0.5, volume: 0.5, color: 123, hex: '#ff0000' }, { selection: 2 }));
  f.sendState(state({ name: 'Old page' }), 'old-session');
  assert.equal(f.element('track-name').textContent, 'Track B');
  assert.equal(f.element('pan-value').textContent, '-0.50');
  assert.equal(f.element('volume-value').textContent, '-6.0 dB');
  assert.equal(f.element('track-color').value, '#ff0000');
  assert.equal(f.requests.length, 0);
  f.sendState(state(false, { selection: 3 }));
  assert.equal(f.element('track-name').textContent, 'No track selected.');
  for (const id of ['pan', 'volume', 'read-fx']) assert.equal(f.element(id).disabled, true);
});

test('continuous snapshots paint track names without overwriting an active pan gesture', async () => {
  const f = await fixture();
  f.element('pan').value = '0.75'; f.element('pan').listeners.input();
  f.sendState(state({ name: 'Renamed A', pan: 0.2 }));
  assert.equal(f.element('track-name').textContent, 'Renamed A');
  assert.equal(f.element('pan').value, 0.75);
  const next = state({ id: 'B', name: 'Track B', pan: -0.5 }, { selection: 2 });
  f.sendState(next);
  assert.equal(f.element('pan').value, -0.5);
  f.reply(f.requests.shift(), { ok: false, error: 'Track selection changed.', state: next }); await flush();
  assert.equal(f.element('pan').value, -0.5);
});

test('Runtime Studio keeps the latest selection when name responses arrive out of order', async () => {
  const label = { textContent: '' }, names = [];
  let id = 0;
  const context = vm.createContext({
    document: { getElementById: () => label },
    reaper: {
      GetSelectedTrack: async () => ({ id: ++id }),
      GetTrackName: () => new Promise(resolve => names.push(resolve))
    }
  });
  const source = fs.readFileSync(path.join(__dirname, '../runtime/runtime-demo/app.js'), 'utf8');
  vm.runInContext(source.slice(0, source.indexOf("action('open'")), context);
  const first = context.selectedTrack(); await flush();
  const second = context.selectedTrack(); await flush();
  names[1]([true, 'Track B']); await second;
  names[0]([true, 'Track A']); await first;
  assert.equal(label.textContent, 'Track B');
});

test('manual project read waits for its response and reports failures without false success', async () => {
  const f = await fixture();
  const pending = f.element('read-project').listeners.click();
  const request = f.requests.shift();
  assert.equal(request.command, 'project'); assert.equal(request.epoch, 1);
  assert.equal(f.element('read-project').disabled, true);
  assert.doesNotMatch(f.element('activity').textContent, /\[PROJECT\]/);
  f.reply({ ...request, id: request.id + 100 }, { data: project }); await flush();
  assert.equal(f.element('read-project').disabled, true);
  f.reply(request, { state: state(), data: { ...project, name: 'Fresh project', cursor: 2.5 } }); await pending;
  assert.equal(f.element('project-name').textContent, 'Fresh project');
  assert.equal(f.element('cursor-position').textContent, '2.500 s');
  assert.equal(f.element('read-project').disabled, false);
  f.element('clear-log').listeners.click();
  const failed = f.element('read-project').listeners.click();
  f.reply(f.requests.shift(), { ok: false, error: 'Project changed.' }); await failed;
  assert.match(f.element('activity').textContent, /\[ERROR\] ERROR: Project changed\./);
  assert.doesNotMatch(f.element('activity').textContent, /\[PROJECT\]/);
  assert.equal(f.element('read-project').disabled, false);
});

test('host Pan changes are coalesced, retain final values and do not repeat unchanged snapshots', async () => {
  const f = await fixture();
  f.sendState(state()); f.sendState(state());
  assert.equal(f.element('activity').children.length, 0);
  for (const pan of [0.1, 0.2, 0.3, 0.4, 0.5]) f.sendState(state({ pan }));
  assert.equal(f.timers.size, 1);
  f.context.flushSliderLog('pan');
  const panLines = f.element('activity').children.filter(row => row.textContent.includes('[PAN]'));
  assert.equal(panLines.length, 1);
  assert.match(panLines[0].textContent, /0\.000 \(center\) → \+0\.500 \(50\.0% R\) · Lua readback/);
  f.sendState(state({ pan: 0.5, name: 'Renamed' }));
  assert.match(f.element('activity').textContent, /\[NAME\] "Track A" → "Renamed"/);
  f.sendState(state({ pan: 0.6, name: 'Renamed' }));
  f.element('clear-log').listeners.click();
  assert.equal(f.timers.size, 0);
  f.context.flushSliderLog('pan');
  assert.equal(f.element('activity').textContent, '');
});

test('slider sends only the latest queued value and logs confirmed Lua readback', async () => {
  const f = await fixture();
  for (const value of ['0.6', '0.7', '0.8']) {
    f.element('pan').value = value; f.element('pan').listeners.input();
  }
  assert.equal(f.requests.length, 1);
  const first = f.requests.shift();
  assert.equal(first.command, 'pan'); assert.equal(first.value, '0.6');
  assert.equal(first.epoch, 1); assert.equal(first.selection, 1);
  assert.equal(f.element('pan-value').textContent, '0.80');
  assert.doesNotMatch(f.element('activity').textContent, /\[PAN\]/);
  f.reply(first, { state: state({ pan: 0.6 }), data: { pan: 0.6 } }); await flush();
  assert.equal(f.requests.length, 1);
  const latest = f.requests.shift(); assert.equal(latest.value, '0.8');
  f.element('pan').listeners.change();
  f.reply(latest, { state: state({ pan: 0.8 }), data: { pan: 0.8 } }); await flush();
  assert.match(f.element('activity').textContent, /\+0\.800.*Lua readback/);
  assert.doesNotMatch(f.element('activity').textContent, /\+0\.700/);
  f.element('pan').value = '0.9'; f.element('pan').listeners.input();
  f.reply(f.requests.shift(), { ok: false, error: 'REAPER rejected the Pan update.', state: state({ pan: 0.8 }) }); await flush();
  assert.match(f.element('activity').textContent, /\[ERROR\] ERROR: REAPER rejected the Pan update/);
  assert.equal(f.element('pan').value, 0.8);
  assert.equal(f.requests.length, 0); assert.equal(f.timers.size, 0);
});

test('debug log is bounded, copies visible history and mirrors only when requested', async () => {
  const f = await fixture();
  for (let i = 0; i < 210; i++) f.context.log(`Entry ${i}`);
  assert.equal(f.element('activity').children.length, 200);
  assert.match(f.element('activity').firstElementChild.textContent, /Entry 10$/);
  await f.element('copy-log').listeners.click();
  assert.equal(f.copies[0].split('\n').length, 200);
  assert.equal(f.requests.length, 0, 'REAPER console is opt-in');
  f.element('log-reaper').checked = true;
  f.context.report({ code: 'API_FAILURE', message: 'Test failure' });
  const mirror = f.requests.shift(); assert.equal(mirror.command, 'console');
  assert.match(mirror.value, /API_FAILURE: Test failure/);
  assert.equal(f.consoleLogs.at(-1).level, 'error');
  f.reply(mirror, { ok: false, error: 'Disconnected' }); await flush();
  assert.equal(f.element('log-reaper').checked, false);
  assert.match(f.element('activity').textContent, /REAPER console output stopped: Disconnected/);
  f.element('clear-log').listeners.click();
  assert.equal(f.element('activity').textContent, '');
  assert.equal(f.element('copy-log').disabled, true);
});

test('Center Pan uses the original selection and a rejected stale write preserves the new track', async () => {
  const f = await fixture();
  const center = f.element('center-pan').listeners.click(); await flush();
  const request = f.requests.shift();
  assert.equal(request.command, 'center-pan'); assert.equal(request.selection, 1);
  const next = state({ id: 'B', name: 'Track B', pan: 0.25 }, { selection: 2 });
  f.sendState(next);
  f.reply(request, { ok: false, error: 'Track selection changed. Try again on the current track.', state: next }); await center;
  assert.equal(f.element('track-name').textContent, 'Track B');
  assert.equal(f.element('pan-value').textContent, '0.25');
  assert.doesNotMatch(f.element('activity').textContent, /\[WRITE\]/);
  assert.match(f.element('activity').textContent, /Track selection changed/);
  const retry = f.element('center-pan').listeners.click(); await flush();
  const current = f.requests.shift(); assert.equal(current.selection, 2);
  f.reply(current, { state: state({ id: 'B', name: 'Track B' }, { selection: 2 }), data: { pan: 0 } }); await retry;
  assert.equal(f.element('pan-value').textContent, '0.00');
  assert.match(f.element('activity').textContent, /\[WRITE\] "Track B" · Center Pan applied/);
});

test('unconfirmed commands time out and bridge rejection is reported', async () => {
  const f = await fixture();
  const pending = f.element('read-project').listeners.click();
  assert.equal(f.timers.size, 1);
  [...f.timers.values()][0](); await pending;
  assert.match(f.element('activity').textContent, /Lua did not confirm "project"/);
  assert.equal(f.element('read-project').disabled, false);
  f.reaper.host.send = () => false;
  await f.element('read-project').listeners.click();
  assert.match(f.element('activity').textContent, /message bridge did not accept/);
  assert.equal(f.timers.size, 0);
});

test('page close cancels pending commands, clears timers and unsubscribes from Lua messages', async () => {
  const f = await fixture();
  f.sendState(state({ pan: 0.5 }));
  const pending = f.element('read-project').listeners.click();
  assert.equal(f.timers.size, 2);
  f.listeners.pagehide(); await pending;
  assert.equal(f.timers.size, 0); assert.equal(f.unsubscribed, true);
  f.sendState(state({ name: 'Ignored after close' }));
  assert.equal(f.element('track-name').textContent, 'Track A');
  assert.doesNotMatch(f.element('activity').textContent, /\[ERROR\]/);
});
