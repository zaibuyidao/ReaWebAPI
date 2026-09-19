const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const flush = () => new Promise(resolve => setImmediate(resolve));
function fixture() {
  const elements = new Map(), reads = [], batches = [], colors = [];
  const element = id => {
    if (!elements.has(id)) elements.set(id, {
      textContent: '', value: '#b7f58a', disabled: false, listeners: {},
      addEventListener(name, fn) { this.listeners[name] = fn; },
      replaceChildren() {}, setAttribute() {}, removeAttribute() {},
      classList: { add() {}, remove() {} }
    });
    return elements.get(id);
  };
  const deferred = (queue, args) => new Promise((resolve, reject) => queue.push({ args, resolve, reject }));
  const reaper = {
    lifecycle: { ready: new Promise(() => {}) },
    GetSelectedTrack: (...args) => deferred(reads, args),
    transaction: { batch: calls => deferred(batches, calls) },
    ColorFromNative: color => deferred(colors, color)
  };
  const document = { getElementById: element, activeElement: null };
  const context = vm.createContext({ document, reaper, window: { reaper, addEventListener() {} }, console });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../demo/app.js'), 'utf8'), context);
  return { context, element, document, reads, batches, colors };
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
