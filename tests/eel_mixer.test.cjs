const assert = require('node:assert/strict');
const {test} = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../web/eel-mixer/app.js'), 'utf8');
const settle = async () => { for (let i = 0; i < 20; i++) await Promise.resolve(); };

async function fixture(demo = 'eel-mixer') {
  const elements = {}, sent = [], timers = new Map(), windowEvents = {}, events = {}, frames = new Map();
  let timerId = 0, transport = () => Promise.resolve(true);
  for (const id of ['controls', 'name', 'volume', 'pan', 'volume-value', 'pan-value', 'mute', 'solo', 'color', 'reset-color', 'color-state', 'error', 'refresh', 'status']) {
    let value = id === 'volume' ? '1' : '0';
    const listeners = {}, attributes = {};
    elements[id] = {id, disabled: id === 'controls', textContent: '', style: {},
      get value() { return value; }, set value(next) { value = String(next); },
      addEventListener: (event, fn) => { listeners[event] = fn; },
      setAttribute: (key, text) => { attributes[key] = text; },
      fire: event => listeners[event]?.(),
    };
  }
  const context = vm.createContext({document: {querySelectorAll: () => Object.values(elements)},
    window: {addEventListener: (name, fn) => { windowEvents[name] = fn; }},
    setTimeout: fn => { const id = ++timerId; timers.set(id, fn); return id; },
    clearTimeout: id => timers.delete(id),
    requestAnimationFrame: fn => { const id = ++timerId; frames.set(id, fn); return id; },
    cancelAnimationFrame: id => frames.delete(id),
    reaper: {events: {on: async (name, fn) => { events[name] = fn; }}, host: {send: text => { sent.push(text.split(' ')); return transport(); }}},
  });
  vm.runInContext(demo === 'eel-mixer' ? source : fs.readFileSync(path.join(__dirname, '../web/eel-backend/app.js'), 'utf8'), context);
  await settle();
  const receive = text => events.message(text);
  const draw = () => { const callbacks = [...frames.values()]; frames.clear(); callbacks.forEach(fn => fn()); };
  const client = sent[0][0];
  const ack = (request, revision, status = 'ok', generation = Number(request[3]), guid = request[4]) =>
    receive(`ack ${client} ${request[1]} ${revision} ${generation} ${guid} ${status} ${Number(request[5]) || 0}`);
  const text = (revision, volume = 1, pan = 0, generation = 1, guid = 'A') =>
    `state ${client} ${revision} ${generation} ${guid} ${volume}${demo === 'eel-mixer' ? ` ${pan} 1 2 3 1 0 0` : ''}\nTrack ${guid}`;
  const snapshot = (...args) => { events.frame({lua: text(...args)}); draw(); };
  ack(sent[0], 1, 'ok', 1, 'A'); snapshot(2); await settle();
  return {elements, sent, timers, ack, snapshot, receive, client, events, frames, draw, text,
    read: code => vm.runInContext(code, context),
    write(key, value, event = 'input') { elements[key].value = value; elements[key].fire(event); },
    transport(fn) { transport = fn; }, close: () => windowEvents.pagehide(),
  };
}

test('execution acknowledgements bound traffic and retain both terminal values under delayed/reordered replies', async () => {
  const f = await fixture();
  for (let i = 0; i < 1000; i++) { f.write('volume', i / 1000); f.write('pan', -i / 1000); }
  f.write('volume', .5, 'change'); f.write('pan', -.37, 'change');
  await settle();
  assert.equal(f.sent.length, 17); // ready + eight unconfirmed writes per control
  assert.equal(f.read('sliders.volume.flights.size'), 8);
  assert.equal(f.read('sliders.volume.queued.value'), .5);
  let drained = false;
  const draining = f.read("Promise.all([drainSlider('volume'), drainSlider('pan')])").then(() => { drained = true; });
  await settle(); assert.equal(drained, false);
  f.snapshot(3, .1, .1);
  assert.equal(f.elements.volume.value, '0.5');
  const flights = f.sent.slice(1);
  for (let i = flights.length - 1; i >= 0; i--) f.ack(flights[i], i + 4, 'superseded');
  await settle();
  assert.equal(f.sent.length, 19);
  const finalVolume = f.sent.slice(17).find(x => x[2] === 'volume');
  const finalPan = f.sent.slice(17).find(x => x[2] === 'pan');
  assert.equal(Number(finalVolume[5]), .5); assert.equal(Number(finalPan[5]), -.37);
  f.ack(finalVolume, 20); await settle(); assert.equal(drained, false);
  f.ack(finalPan, 21); await draining;
  f.snapshot(19, .1, .1); // Old state delivered after the final ack must not win.
  assert.equal(f.elements.volume.value, '0.5'); assert.equal(f.elements.pan.value, '-0.37');
  f.snapshot(22, .5, -.37);
  f.snapshot(23, .75, .62); // External changes still flow back after the gesture.
  assert.equal(f.elements.volume.value, '0.75'); assert.equal(f.elements.pan.value, '0.62');
  assert.equal(f.read('pending.size'), 0); assert.equal(f.timers.size, 0);
});

for (const demo of ['eel-backend', 'eel-mixer']) {
test(`${demo}: keyboard edits and stale contexts cannot overwrite a new track gesture`, async () => {
  const f = await fixture(demo);
  f.write('volume', .4); // No pointerdown: keyboard input is protected too.
  f.snapshot(3, .1); assert.equal(f.elements.volume.value, '0.4');
  const old = f.sent.at(-1);
  f.snapshot(4, 1, 0, 2, 'B'); assert.equal(f.elements.volume.value, '1');
  f.write('volume', .7, 'change'); const next = f.sent.at(-1);
  f.ack(old, 5, 'stale', 2, 'B'); await settle();
  assert.equal(f.elements.volume.value, '0.7'); assert.equal(f.elements.error.textContent, '');
  f.ack(next, 6); await settle(); f.snapshot(7, .7, 0, 2, 'B');
  f.snapshot(8, .4, 0, 1, 'A'); assert.equal(f.elements.volume.value, '0.7');
});

test(`${demo}: queue rejection, readback failure and missing execution ack are explicit failures, never blind retries`, async () => {
  for (const failure of ['queue', 'readback', 'timeout']) {
    const f = await fixture(demo);
    if (failure === 'queue') f.transport(() => Promise.resolve(false));
    f.write('volume', .6, 'change'); await settle();
    if (failure === 'readback') f.ack(f.sent.at(-1), 3, 'readback');
    if (failure === 'timeout') for (const fn of [...f.timers.values()]) fn();
    await settle();
    assert.equal(f.sent.length, 2); assert.equal(f.read('pending.size'), 0);
    assert.equal(f.read('sliders.volume.flights.size'), 0);
    assert.match(f.elements.error.textContent, /rejected|did not confirm/);
    assert.equal(f.timers.size, 0);
  }
});

test(`${demo}: foreign document replies cannot settle requests and page teardown cancels pending work`, async () => {
  const f = await fixture(demo);
  for (let i = 0; i < 30; i++) f.write('volume', i / 30);
  const request = f.sent.at(-1);
  f.receive(`ack previous-document ${request[1]} 100 1 A ok 1`);
  await settle(); assert.equal(f.read('sliders.volume.flights.size'), 8);
  const count = f.sent.length; f.close(); await settle();
  assert.equal(f.read('pending.size'), 0); assert.equal(f.timers.size, 0);
  assert.equal(f.read('sliders.volume.queued'), null); assert.equal(f.sent.length, count);
  assert.equal(f.elements.error.textContent, '');
});
}

for (const demo of ['eel-backend', 'eel-mixer']) {
  test(`${demo}: frame state, 0..4 gain and final-value protection across channel ordering`, async () => {
    const f = await fixture(demo);
    f.receive(f.text(100, 2)); // State must arrive on the frame channel.
    assert.equal(f.elements.volume.value, '1');
    for (let i = 0; i < 1000; i++) f.write('volume', i / 250);
    f.write('volume', 4, 'change');
    await settle();
    assert.equal(f.sent.length, 9);
    assert.equal(f.read('sliders.volume.queued.value'), 4);
    for (const [i, request] of f.sent.slice(1).entries()) f.ack(request, i + 3, 'superseded');
    await settle();
    const final = f.sent.at(-1);
    assert.equal(Number(final[5]), 4);
    f.events.frame({lua: f.text(11, .2)}); // Draw delayed until after final ack.
    f.ack(final, 12); await settle(); f.draw();
    assert.equal(f.elements.volume.value, '4');
    f.snapshot(13, 4);
    f.write('volume', 0, 'change');
    const zero = f.sent.at(-1);
    f.snapshot(15, 0); // New frame may arrive before its execution ack.
    f.ack(zero, 14); await settle();
    assert.equal(f.elements.volume.value, '0');
    f.events.frame({lua: f.text(16, 2)});
    f.events.frame({lua: f.text(17, 3.5)});
    assert.equal(f.frames.size, 1);
    f.draw(); assert.equal(f.elements.volume.value, '3.5');
    f.snapshot(16, .1); assert.equal(f.elements.volume.value, '3.5');
    f.events.frame({lua: f.text(18, .2)});
    f.close(); await settle();
    assert.equal(f.frames.size, 0); assert.equal(f.timers.size, 0);
    assert.equal(f.elements.volume.value, '3.5');
    const eel = fs.readFileSync(path.join(__dirname, `../web/${demo}/reawebapi-${demo}.eel`), 'utf8');
    assert.match(eel, /ReaWeb_SetFrame\(window_id, #out\)/);
    assert.doesNotMatch(eel, /extension_api\(|next_poll/);
    assert.match(fs.readFileSync(path.join(__dirname, `../web/${demo}/index.html`), 'utf8'), /min="0" max="4"/);
  });
}
