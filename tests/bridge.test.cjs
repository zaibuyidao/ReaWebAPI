const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const { webcrypto } = require('node:crypto');
const methods = Object.keys(JSON.parse(fs.readFileSync(path.join(__dirname, '../api/bindings.json'), 'utf8')).functions);
const script = '(() => {\n' + fs.readFileSync(path.join(__dirname, '../runtime/reaper-api.generated.js'), 'utf8') + fs.readFileSync(path.join(__dirname, '../runtime/reaper.js'), 'utf8') + '\n})();';
const flush = () => new Promise(setImmediate);
function setup(engine = 'windows') {
  const messages = [], timers = new Map(), listeners = {};
  let timerId = 0;
  const post = message => messages.push(JSON.parse(message));
  const window = { addEventListener: (event, handler) => { listeners[event] = handler; } };
  window.top = window;
  if (engine === 'windows') window.chrome = { webview: { postMessage: post } };
  else if (engine === 'webkit') window.webkit = { messageHandlers: { reaweb: { postMessage: post } } };
  const context = vm.createContext({ window, crypto: webcrypto, TextEncoder, console, Uint8Array, Float64Array, DataView, btoa, atob,
    setTimeout: fn => { timers.set(++timerId, fn); return timerId; }, clearTimeout: id => timers.delete(id) });
  vm.runInContext(script, context);
  const reply = (index, payload) => window.__reawebReceive({ id: messages[index].id, document: messages[index].document, ...payload });
  const event = (name, sequence, data) => window.__reawebReceive({ document: messages[0].document, event: name, sequence, data });
  return { window, messages, timers, listeners, reply, event, context };
}
async function connected(engine) {
  const t = setup(engine);
  t.reply(0, { result: { protocol: 1, projectEpoch: 1, methods } });
  await t.window.reaper.ready;
  return t;
}
for (const engine of ['windows', 'webkit']) {
  test(`${engine}: handshake gates calls, out-of-order replies preserve promises`, async () => {
    const t = setup(engine);
    const first = t.window.reaper.GetSelectedTrack(0, 0);
    const second = t.window.reaper.CountTracks(0);
    assert.equal(t.messages.length, 1);
    assert.equal(t.messages[0].method, '__reawebHello');
    t.reply(0, { result: { protocol: 1, projectEpoch: 1, methods } });
    await flush();
    t.reply(2, { result: 3 });
    t.reply(1, { result: { type: 'MediaTrack', id: '1:1' } });
    assert.equal(await second, 3);
    assert.equal((await first).id, '1:1');
    const failed = assert.rejects(t.window.reaper.GetTrackName(null), { code: 'INVALID_HANDLE' });
    await flush();
    t.reply(3, { error: { code: 'INVALID_HANDLE', message: 'Expected track' } });
    await failed;
    assert.equal(t.timers.size, 0);
    assert.ok(Object.isFrozen(t.window.reaper));
  });
}
test('timeouts, page close and pre-handshake backpressure settle every promise', async () => {
  const t = await connected();
  const timeout = assert.rejects(t.window.reaper.CountTracks(), { code: 'TIMEOUT' });
  await flush();
  for (const timer of [...t.timers.values()]) timer();
  await timeout; t.timers.clear();
  const closed = assert.rejects(t.window.reaper.CountTracks(), { code: 'WINDOW_CLOSED' });
  t.listeners.pagehide(); await closed;
  await assert.rejects(t.window.reaper.CountTracks(), { code: 'WINDOW_CLOSED' });
  const early = setup();
  const pending = Array.from({ length: 256 }, () => assert.rejects(early.window.reaper.CountTracks(), { code: 'WINDOW_CLOSED' }));
  await assert.rejects(early.window.reaper.CountTracks(), { code: 'QUEUE_LIMIT' });
  early.listeners.pagehide(); await Promise.all(pending);
  assert.equal(early.timers.size, 0);
});
test('old document replies are ignored, project errors retain details and update epoch', async () => {
  const t = await connected();
  const pending = assert.rejects(t.window.reaper.CountTracks(), { code: 'PROJECT_CHANGED', details: { projectEpoch: 2 } });
  await flush();
  t.window.__reawebReceive({ id: t.messages[1].id, document: 'old', result: 999 });
  assert.equal(t.timers.size, 1);
  t.reply(1, { error: { code: 'PROJECT_CHANGED', message: 'Changed', details: { projectEpoch: 2 } } });
  await pending;
  const next = t.window.reaper.CountTracks(); await flush();
  assert.equal(t.messages[2].project, 2);
  t.reply(2, { result: 4 }); assert.equal(await next, 4);
});
test('subscriptions share native registration, order events and dispose independently', async () => {
  const t = await connected(), states = [], more = [];
  const off1 = t.window.reaper.ReaWeb_On('windowstatechange', s => states.push(s.docked));
  const off2 = t.window.reaper.ReaWeb_On('windowstatechange', s => more.push(s.docked));
  await flush(); assert.equal(t.messages.length, 2);
  t.reply(1, { result: { docked: false } });
  const dispose1 = await off1, dispose2 = await off2;
  t.event('windowstatechange', 2, { docked: true });
  t.event('windowstatechange', 1, { docked: false });
  assert.deepEqual(states, [false, true]); assert.deepEqual(more, [false, true]);
  await dispose1(); assert.equal(t.messages.length, 2);
  const finish = dispose2(); await flush();
  assert.equal(t.messages[2].method, 'ReaWeb_Unsubscribe');
  t.reply(2, { result: true }); await finish; await dispose2();
  t.event('windowstatechange', 3, { docked: false });
  assert.deepEqual(more, [false, true]);
});
test('continuous setter only supersedes waiting values and ordinary setters stay independent', async () => {
  const t = await connected(), track = { type: 'MediaTrack', id: '1:1' };
  const first = t.window.reaper.ReaWeb_SetTrackValueLatest(track, 'D_VOL', .1);
  const middle = t.window.reaper.ReaWeb_SetTrackValueLatest(track, 'D_VOL', .2);
  const last = t.window.reaper.ReaWeb_SetTrackValueLatest(track, 'D_VOL', .3);
  assert.equal((await middle).superseded, true);
  await flush(); assert.equal(t.messages.length, 2); assert.equal(t.messages[1].args[2], .1);
  t.reply(1, { result: true }); assert.equal((await first).applied, true);
  await flush(); assert.equal(t.messages[2].args[2], .3);
  t.reply(2, { result: true }); assert.equal((await last).superseded, false);
  const a = t.window.reaper.SetMediaTrackInfo_Value(track, 'D_VOL', .4);
  const b = t.window.reaper.SetMediaTrackInfo_Value(track, 'D_VOL', .5);
  await flush(); assert.equal(t.messages.length, 5);
  t.reply(3, { result: true }); t.reply(4, { result: true }); await Promise.all([a,b]);
});
test('limits, serialization, unavailable host and protocol mismatch fail cleanly', async () => {
  const t = await connected();
  await assert.rejects(t.window.reaper.ReaWebOpen('x'.repeat(64*1024*1024)), { code: 'MESSAGE_LIMIT' });
  await assert.rejects(t.window.reaper.CountTracks(1n), { name: 'TypeError' });
  assert.equal(t.timers.size, 0);
  const absent = setup('none'); await assert.rejects(absent.window.reaper.CountTracks(), { code: 'NO_RUNTIME' });
  const mismatch = setup(); const rejected = assert.rejects(mismatch.window.reaper.CountTracks(), { code: 'PROTOCOL_MISMATCH' });
  mismatch.reply(0, { result: { protocol: 2 } }); await rejected;
  const original = t.window.reaper; vm.runInContext(script, t.context); assert.equal(t.window.reaper, original);
  const iframe = { top: {} }; vm.runInNewContext(script, { window: iframe }); assert.equal(iframe.reaper, undefined);
});

test('generated names expose only callable bindings and reject schema mismatch', async () => {
  const t = await connected();
  for (const name of methods) assert.equal(typeof t.window.reaper[name], 'function');
  assert.equal(typeof t.window.reaper.InsertTrackAtIndex, 'function');
  assert.equal(methods.length,730);
  const mismatch = setup();
  const failed = assert.rejects(mismatch.window.reaper.CountTracks(), { code: 'SCHEMA_MISMATCH' });
  mismatch.reply(0, { result: { protocol: 1, projectEpoch: 1, methods: ['GetAppVersion'] } });
  await failed;
});

test('generated void methods resolve undefined and tuple methods preserve all outputs', async () => {
  const t = await connected();
  const move = t.window.reaper.SetEditCurPos(5, true, false);
  await flush(); t.reply(1, { result: null }); assert.equal(await move, undefined);
  const markers = t.window.reaper.CountProjectMarkers(0);
  await flush(); t.reply(2, { result: [3, 2, 1] });
  assert.deepEqual(await markers, [3, 2, 1]);
});

test('binary payloads preserve all bytes and sample buffers are written back', async () => {
  const t=await connected();
  const allBytes=Uint8Array.from({length:256},(_,i)=>i);
  const sent=t.window.reaper.MIDI_SetAllEvts(null,allBytes);
  await flush();
  assert.equal(t.messages[1].args[1].__reawebBytes, Buffer.from(allBytes).toString('base64'));
  t.reply(1,{result:true}); await sent;
  const got=t.window.reaper.MIDI_GetAllEvts(null); await flush();
  t.reply(2,{result:[true,{__reawebBytes: Buffer.from(allBytes).toString('base64')}]});
  assert.deepEqual((await got)[1],allBytes);
  const samples=new Float64Array(4);
  const read=t.window.reaper.GetAudioAccessorSamples(null,48000,1,0,4,samples);await flush();
  assert.equal(t.messages[3].args[5].__reawebFloat64,Buffer.alloc(32).toString('base64'));
  const samplesOut=Buffer.alloc(32);[-1,.25,.5,NaN].forEach((v,i)=>samplesOut.writeDoubleLE(v,i*8));
  t.reply(3,{result:{__reawebCall:true,value:1,arrays:[{index:5,values:{__reawebBytes:samplesOut.toString('base64')}}]}});
  assert.equal(await read,1);assert.deepEqual(Array.from(samples),[-1,.25,.5,NaN]);
});

test('a started native dialog stops the queue timer but waits for its result', async()=>{
 const t=await connected();let done=false;
 const waiting=t.window.reaper.GetUserInputs('Test',1,'Field','').then(x=>{done=true;return x;});
 await flush();t.reply(1,{started:true});await flush();
 assert.equal(t.timers.size,0);assert.equal(done,false);
 t.reply(1,{result:[true,'answered later']});assert.deepEqual(await waiting,[true,'answered later']);
});

test('managed Undo pairs cleanup on success and callback errors', async () => {
  const t = await connected();
  const value = t.window.reaper.ReaWeb_WithUndo('Edit', async () => 42);
  await flush(); assert.equal(t.messages[1].method, 'ReaWeb_BeginUndo');
  t.reply(1, {result: 'token'}); await flush();
  assert.equal(t.messages[2].method, 'ReaWeb_EndUndo');
  assert.equal(t.messages[2].args[0], 'token');
  t.reply(2, {result: true}); assert.equal(await value, 42);
  const failure = assert.rejects(t.window.reaper.ReaWeb_WithUndo('Fail', async () => { throw new Error('original'); }), /original/);
  await flush(); t.reply(3, {result: 'second'}); await flush();
  t.reply(4, {error: {code:'STALE_UNDO', message:'closed'}});
  await failure;
});

test('host I/O supports large text', async () => {
  const t = await connected();
  const writing = t.window.reaper.ReaWeb_WriteFile('large.txt', 'x'.repeat(100000));
  await flush(); assert.equal(t.messages[1].args[1].length, 100000);
  t.reply(1, {result:{bytes:100000}}); assert.equal((await writing).bytes, 100000);
});

for (const name of ['itemselectionchange', 'takeselectionchange', 'transportchange', 'fxchange']) {
  test(name + ': subscribes and releases its native observer', async () => {
    const t = await connected(), received = [];
    const subscription = t.window.reaper.ReaWeb_On(name, state => received.push(state));
    await flush(); t.reply(1, {result:null}); const dispose = await subscription;
    t.event(name, 1, {revision:1}); assert.equal(received[0].revision, 1);
    const closed = dispose(); await flush(); assert.equal(t.messages[2].method, 'ReaWeb_Unsubscribe');
    t.reply(2, {result:true}); await closed;
  });
}
