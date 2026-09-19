const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const flush = () => new Promise(resolve => setImmediate(resolve));
function fixture() {
  const elements = new Map(), reads = [], batches = [], colors = [], writes = [], copies = [], hostLogs = [], consoleLogs = [], timers = new Map();
  let timerId = 0;
  const node = () => ({
    value: '#b7f58a', disabled: false, checked: false, listeners: {}, children: [],
    scrollHeight: 100, scrollTop: 0, clientHeight: 100, text: '',
    get textContent() { return this.text + this.children.map(child => child.textContent).join('\n'); },
    set textContent(value) { this.text = value; this.children = []; },
    get firstElementChild() { return this.children[0]; },
    append(child) { this.children.push(child); child.remove = () => { this.children.splice(this.children.indexOf(child), 1); }; },
    replaceChildren(...children) { this.text = ''; this.children = []; children.forEach(child => this.append(child)); },
    addEventListener(name, fn) { this.listeners[name] = fn; },
    setAttribute() {}, removeAttribute() {}, classList: { add() {}, remove() {} }
  });
  const element = id => {
    if (!elements.has(id)) elements.set(id, node());
    return elements.get(id);
  };
  const deferred = (queue, args) => new Promise((resolve, reject) => queue.push({ args, resolve, reject }));
  const reaper = {
    lifecycle: { ready: new Promise(() => {}) },
    GetSelectedTrack: (...args) => deferred(reads, args),
    transaction: { batch: calls => deferred(batches, calls) },
    ColorFromNative: color => deferred(colors, color),
    audio: { setTrackValueLatest: (...args) => deferred(writes, args) },
    clipboard: { writeText: async text => { copies.push(text); } },
    debug: Object.fromEntries(['log', 'warn', 'error'].map(level => [level, async text => { hostLogs.push({ level, text }); }]))
  };
  const document = { getElementById: element, createElement: node, activeElement: null };
  const context = vm.createContext({ document, reaper, window: { reaper, addEventListener() {} },
    console: Object.fromEntries(['log', 'warn', 'error'].map(level => [level, (...values) => consoleLogs.push({ level, values })])),
    setTimeout: fn => { timers.set(++timerId, fn); return timerId; }, clearTimeout: id => timers.delete(id) });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../demo/app.js'), 'utf8'), context);
  return { context, element, document, reaper, reads, batches, colors, writes, copies, hostLogs, consoleLogs, timers };
}
async function selected(f, id) {
  f.reads.shift().resolve(id ? { id } : null);
  await flush();
  return f.batches.shift();
}
test('track name and pan render without waiting for native color conversion', async () => {
  const f = fixture(), pending = f.context.refresh(true);
  const batch = await selected(f, 'A');
  assert.equal(batch.args.map(call => call.method).join(','),
    'CountTracks,GetTrackName,GetMediaTrackInfo_Value,GetTrackColor');
  batch.resolve([2, [true, 'Track A'], 0.25, 123]);
  await pending;
  assert.equal(f.element('track-name').textContent, 'Track A');
  assert.equal(f.element('pan-value').textContent, '0.25');
  assert.equal(f.colors.length, 1);
  const oldColor = f.colors.shift();
  const next = f.context.refresh(true);
  assert.equal(f.element('pan').disabled, true);
  (await selected(f, 'B')).resolve([2, [true, 'Track B'], -0.5, 0]);
  await next;
  oldColor.resolve([255, 0, 0]); await flush();
  assert.equal(f.element('track-name').textContent, 'Track B');
  assert.equal(f.element('track-color').value, '#b7f58a');
  assert.equal(f.element('pan-value').textContent, '-0.50');
});
test('rapid selection changes discard stale reads and handle deselection', async () => {
  const f = fixture(), pending = f.context.refresh(true);
  const oldBatch = await selected(f, 'A');
  await f.context.refresh(true);
  oldBatch.resolve([2, [true, 'Track A'], 0.25, 123]); await flush();
  assert.notEqual(f.element('track-name').textContent, 'Track A');
  (await selected(f, 'B')).resolve([2, [true, 'Track B'], 0, 0]);
  await pending;
  assert.equal(f.element('track-name').textContent, 'Track B');
  const empty = f.context.refresh(true);
  (await selected(f, null)).resolve([2]); await empty;
  assert.equal(f.element('track-name').textContent, 'No track selected.');
  assert.equal(f.element('pan').disabled, true);
});
test('continuous project refreshes paint completed reads instead of starving the UI', async () => {
  const f = fixture(), pending = f.context.refresh();
  const batch = await selected(f, 'A');
  await f.context.refresh();
  batch.resolve([2, [true, 'Track A'], 0.1, 0]); await flush();
  assert.equal(f.element('track-name').textContent, 'Track A');
  assert.equal(f.element('pan-value').textContent, '0.10');
  f.document.activeElement = f.element('pan');
  f.element('pan').value = 0.75;
  (await selected(f, 'A')).resolve([2, [true, 'Renamed A'], 0.2, 0]); await pending;
  assert.equal(f.element('track-name').textContent, 'Renamed A');
  assert.equal(f.element('pan').value, 0.75);
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

test('Log selected track waits for a fresh read even when an automatic refresh is pending', async () => {
  const f = fixture(), pending = f.context.refresh();
  const old = await selected(f, 'A');
  f.element('read-track').listeners.click();
  assert.equal(f.element('read-track').disabled, true);
  old.resolve([2, [true, 'Old track'], 0, 0]); await flush();
  assert.doesNotMatch(f.element('activity').textContent, /\[READ\]/);
  (await selected(f, 'B')).resolve([2, [true, 'Guitar "B"'], -0.25, 0]);
  await pending;
  assert.match(f.element('activity').textContent, /\[READ\] Track "Guitar \\"B\\"" · Pan -0\.250 \(25\.0% L\)/);
  assert.equal(f.element('read-track').disabled, false);
  assert.match(f.element('read-track-result').textContent, /^Logged: Track/);
  assert.equal(f.hostLogs.length, 0, 'REAPER console is opt-in');
});

test('manual read reports empty selection and interrupted reads without a false success', async () => {
  const f = fixture();
  f.element('read-track').listeners.click();
  (await selected(f, null)).resolve([0]); await flush();
  assert.match(f.element('activity').textContent, /\[READ\] No track selected\./);
  f.element('clear-log').listeners.click();
  f.element('read-track').listeners.click();
  f.reads.shift().reject({ code: 'STALE_HANDLE', message: 'Expired' }); await flush();
  assert.match(f.element('activity').textContent, /\[READ\] Track read interrupted: STALE_HANDLE/);
  assert.doesNotMatch(f.element('read-track-result').textContent, /^Logged:/);
  assert.equal(f.element('read-track').disabled, false);
});

test('host Pan changes are coalesced, retain final values and do not repeat unchanged snapshots', async () => {
  const f = fixture();
  async function sample(pan, name = 'Track A') {
    const pending = f.context.refresh();
    (await selected(f, 'A')).resolve([1, [true, name], pan, 0]); await pending;
  }
  await sample(0); await sample(0);
  assert.equal(f.element('activity').children.length, 1);
  for (const pan of [0.1, 0.2, 0.3, 0.4, 0.5]) await sample(pan);
  assert.equal(f.timers.size, 1);
  f.context.flushPanLog();
  const panLines = f.element('activity').children.filter(row => row.textContent.includes('[PAN]'));
  assert.equal(panLines.length, 1);
  assert.match(panLines[0].textContent, /0\.000 \(center\) → \+0\.500 \(50\.0% R\) · REAPER readback/);
  await sample(0.5, 'Renamed');
  assert.match(f.element('activity').textContent, /\[NAME\] "Track A" → "Renamed"/);
  await sample(0.6);
  f.element('clear-log').listeners.click();
  assert.equal(f.timers.size, 0);
  f.context.flushPanLog();
  assert.equal(f.element('activity').textContent, '');
});

test('slider logs the value read back from REAPER, and rejects failed writes', async () => {
  const f = fixture(), initial = f.context.refresh();
  (await selected(f, 'A')).resolve([1, [true, 'Track A'], 0, 0]); await initial;
  f.element('pan').value = '0.6'; f.element('pan').listeners.input();
  assert.equal(f.element('pan-value').textContent, '0.60');
  assert.equal(f.writes[0].args[2], 0.6);
  f.writes.shift().resolve({ applied: true, superseded: false }); await flush();
  (await selected(f, 'A')).resolve([1, [true, 'Track A'], 0.5, 0]); await flush();
  f.context.flushPanLog();
  const lines = f.element('activity').textContent;
  assert.match(lines, /\+0\.500/);
  assert.doesNotMatch(lines, /\+0\.600/);
  f.element('pan').value = '0.7'; f.element('pan').listeners.input();
  f.writes.shift().resolve({ applied: false, superseded: false }); await flush();
  assert.match(f.element('activity').textContent, /\[ERROR\] ERROR: REAPER rejected the Pan update/);
  assert.equal(f.reads.length, 0);
});

test('debug log is bounded, copies visible history and mirrors only when requested', async () => {
  const f = fixture();
  for (let i = 0; i < 210; i++) f.context.log(`Entry ${i}`);
  assert.equal(f.element('activity').children.length, 200);
  assert.match(f.element('activity').firstElementChild.textContent, /Entry 10$/);
  await f.element('copy-log').listeners.click();
  assert.equal(f.copies[0].split('\n').length, 200);
  assert.equal(f.hostLogs.length, 0);
  f.element('log-reaper').checked = true;
  f.context.report({ code: 'API_FAILURE', message: 'Test failure' }); await flush();
  assert.equal(f.hostLogs[0].level, 'error');
  assert.match(f.hostLogs[0].text, /API_FAILURE: Test failure/);
  assert.equal(f.consoleLogs.at(-1).level, 'error');
  f.reaper.debug.log = async () => { throw new Error('Disconnected'); };
  f.context.log('Mirror failure'); await flush();
  assert.equal(f.element('log-reaper').checked, false);
  assert.match(f.element('activity').textContent, /REAPER console output stopped: Disconnected/);
  f.element('clear-log').listeners.click();
  assert.equal(f.element('activity').textContent, '');
  assert.equal(f.element('copy-log').disabled, true);
});

test('Center Pan completion names its original track and cannot overwrite a new selection', async () => {
  const f = fixture(), initial = f.context.refresh();
  (await selected(f, 'A')).resolve([2, [true, 'Track A'], 0.5, 0]); await initial;
  const center = f.element('center-pan').listeners.click(); await flush();
  const write = f.batches.shift();
  const changed = f.context.refresh(true);
  (await selected(f, 'B')).resolve([2, [true, 'Track B'], 0.25, 0]); await changed;
  write.resolve([true]); await center;
  assert.equal(f.element('pan-value').textContent, '0.25');
  assert.match(f.element('activity').textContent, /\[WRITE\] "Track A" · Center Pan applied/);
});
