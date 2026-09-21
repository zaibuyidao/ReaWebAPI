const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const { webcrypto } = require('node:crypto');
const methods = Object.keys(JSON.parse(fs.readFileSync(path.join(__dirname, '../api/bindings.json'), 'utf8')).functions);
const batchMethods = [...fs.readFileSync(path.join(__dirname, '../src/core/batch.hpp'), 'utf8').matchAll(/"([A-Za-z][A-Za-z0-9_]+)"/g)].map(match => match[1]);
const script = '(() => {\n' + fs.readFileSync(path.join(__dirname, '../runtime/reaper-api.generated.js'), 'utf8') + fs.readFileSync(path.join(__dirname, '../runtime/reaper.js'), 'utf8') + '\n})();';
const flush = () => new Promise(setImmediate);

test('The public SDK has exactly 730 unchanged Mirror methods and thirteen frozen Runtime namespaces', async () => {
  const t = await connected();
  const api = t.window.reaper;
  const expected = {
  "window": [
    "open",
    "openDev",
    "getSize",
    "setSize",
    "getPosition",
    "setPosition",
    "show",
    "hide",
    "getState",
    "setTitle",
    "focus",
    "setDocked",
    "setIcon",
    "setIconVisible",
    "isDocked",
    "setKeyboardCapture",
    "close",
    "reload"
  ],
  "theme": [
    "getColors",
    "apply"
  ],
  "dialog": [
    "openFile",
    "saveFile",
    "selectFolder"
  ],
  "events": [
    "off",
    "on"
  ],
  "lifecycle": [
    "ready",
    "on"
  ],
  "debug": [
    "log",
    "warn",
    "error",
    "inspect",
    "getLogs",
    "getDiagnostics",
    "openDevTools",
    "setBufferSize"
  ],
  "fs": [
    "readText",
    "writeText",
    "readBinary",
    "writeBinary",
    "readFile",
    "writeFile",
    "stat",
    "readDirectory",
    "makeDirectory"
  ],
  "audio": [
    "getFileInfo",
    "getWaveform",
    "getTrackMeter",
    "setTrackValueLatest"
  ],
  "clipboard": [
    "readText",
    "writeText"
  ],
  "dragDrop": ["startFiles", "startText"],
  "app": ["getId", "getName", "getVersion", "getRootPath", "getDataPath"],
  "system": ["getPlatform", "getArchitecture", "revealInFileManager",
    "openExternal",
    "getCapabilities"
  ],
  "transaction": [
    "batch",
    "beginUndo",
    "endUndo",
    "withUndo"
  ]
};
  assert.equal(methods.length, 730);
  assert.deepEqual(Object.keys(api).sort(), [...methods, ...Object.keys(expected)].sort());
  assert.equal(Object.getPrototypeOf(api), null);
  assert.equal(Object.hasOwn(api, 'edit'), false);
  for (const namespace of ['app', 'dragDrop']) {
    assert.equal(Reflect.set(api[namespace], 'placeholder', () => {}), false);
  }
  for (const [namespace, names] of Object.entries(expected)) {
    assert.ok(Object.isFrozen(api[namespace]));
    assert.deepEqual(Object.keys(api[namespace]).sort(), names.sort());
    for (const name of names) {
      if (namespace === 'lifecycle' && name === 'ready') assert.equal(typeof api[namespace][name].then, 'function');
      else assert.equal(typeof api[namespace][name], 'function');
    }
  }
  for (const name of methods) assert.equal(typeof api[name], 'function');
  const inventory = fs.readFileSync(path.join(__dirname, '../docs/runtime-api-inventory.md'), 'utf8');
  const documented = [...inventory.matchAll(/^\| `reaper\.(\w+)\.(\w+)[(`]/gm)].map(match => match[1] + '.' + match[2]);
  const declared = Object.entries(expected).flatMap(([ns,names]) => names.map(name => ns + '.' + name));
  assert.deepEqual(documented.sort(), declared.sort());
  const headings = [...inventory.matchAll(/^## reaper\.(\w+)$/gm)].map(match => match[1]);
  assert.deepEqual(headings.sort(), Object.keys(expected).sort());
});

test('Encoding-specific fs helpers preserve bytes, overwrite policy, and host failures', async () => {
  const t = await connected(), io = t.window.reaper.fs;
  assert.equal(await answer(t, 'ReaWeb_ReadFile', ['text.txt',{encoding:'utf8'}], '你好', io.readText('text.txt')), '你好');
  const bytes = new Uint8Array([0,128,255]);
  assert.deepEqual(await answer(t, 'ReaWeb_ReadFile', ['bytes.bin',{encoding:'binary'}],
    {__reawebBytes: btoa(String.fromCharCode(...bytes))}, io.readBinary('bytes.bin')), bytes);
  await answer(t, 'ReaWeb_WriteFile', ['text.txt','hello',{encoding:'utf8'}], {path:'text.txt',bytes:5}, io.writeText('text.txt','hello'));
  await answer(t, 'ReaWeb_WriteFile', ['bytes.bin',{__reawebBytes: btoa(String.fromCharCode(...bytes))},{overwrite:true,encoding:'binary'}],
    {path:'bytes.bin',bytes:3}, io.writeBinary('bytes.bin',bytes,{overwrite:true}));
  const failed = assert.rejects(io.readText('missing.txt'), {code:'FILE_NOT_FOUND'});
  await flush();
  t.reply(t.messages.length-1,{error:{code:'FILE_NOT_FOUND',message:'Missing file'}});
  await failed;
});

test('Migrated host services keep their native argument and result contracts', async () => {
  const t = await connected(), api = t.window.reaper;
  const cases = [
    ['window','openDev','ReaWeb_OpenDev',['http://localhost:5173/'],9],
    ['system','openExternal','ReaWeb_OpenExternal',['https://example.com'],true],
    ['window','setDocked','ReaWeb_SetDocked',[false],false],
    ['window','setIcon','ReaWeb_SetIcon',['logo.svg'],true],
    ['window','setIconVisible','ReaWeb_SetIconVisible',[true],true],
    ['window','setIconVisible','ReaWeb_SetIconVisible',[false],true],
    ['window','isDocked','ReaWeb_IsDocked',[],false],
    ['window','setKeyboardCapture','ReaWeb_SetKeyboardCapture',[false],false],
    ['system','getCapabilities','ReaWeb_GetCapabilities',[],{runtime:{contract:2}}],
    ['debug','setBufferSize','ReaWeb_SetBufferSize',[131072],131072],
    ['fs','stat','ReaWeb_Stat',['folder'],{path:'folder',exists:true,type:'directory',size:null}],
    ['fs','readDirectory','ReaWeb_ReadDirectory',['folder'],[]],
    ['fs','makeDirectory','ReaWeb_MakeDirectory',['nested/folder',{recursive:true}],true],
    ['clipboard','readText','ReaWeb_ClipboardReadText',[],'clipboard'],
    ['clipboard','writeText','ReaWeb_ClipboardWriteText',['clipboard'],true],
    ['transaction','batch','ReaWeb_Batch',[[{method:'CountTracks',args:[0]}],{undoLabel:'Count'}],[3]]
  ];
  for (const [namespace,name,wire,args,result] of cases)
    assert.deepEqual(JSON.parse(JSON.stringify(await answer(t,wire,args,result,api[namespace][name](...args)))), result);
});

async function answer(t, method, args, value, promise) {
  await flush();
  const index = t.messages.length - 1;
  assert.equal(t.messages[index].method, method);
  assert.deepEqual(t.messages[index].args, args);
  t.reply(index, { result: value });
  return promise;
}

test('App getters and system/native drag methods preserve native values, cancellation and errors', async () => {
  const t = await connected(), api = t.window.reaper;
  const info = {id:'app-123',name:'SendFlow',version:null,rootPath:'C:/Tools/SendFlow',dataPath:'C:/REAPER/Apps/app-123/Data'};
  for (const [getter, key] of [['getId','id'],['getName','name'],['getVersion','version'],['getRootPath','rootPath'],['getDataPath','dataPath']])
    assert.equal(await answer(t,'ReaWeb_GetAppInfo',[],info,api.app[getter]()),info[key]);
  await answer(t,'ReaWeb_GetPlatform',[],'windows',api.system.getPlatform());
  await answer(t,'ReaWeb_GetArchitecture',[],'arm64',api.system.getArchitecture());
  await answer(t,'ReaWeb_RevealPath',['sample.wav'],true,api.system.revealInFileManager('sample.wav'));
  await answer(t,'ReaWeb_DragFiles',[['sample.wav']],false,api.dragDrop.startFiles(['sample.wav']));
  await answer(t,'ReaWeb_DragText',['text'],true,api.dragDrop.startText('text'));
  const rejected = assert.rejects(api.dragDrop.startText('no gesture'), {code:'DRAG_GESTURE_REQUIRED'});
  await flush(); t.reply(t.messages.length-1,{error:{code:'DRAG_GESTURE_REQUIRED',message:'Hold mouse'}}); await rejected;
  assert.equal(api.debug.info, undefined); assert.equal(api.dragdrop, undefined);
});

test('events.off removes matching duplicates during registration, preserving other listeners and fresh subscriptions', async () => {
  const t = await connected(), events = t.window.reaper.events;
  let removed = 0, kept = 0;
  const callback = () => ++removed, other = () => ++kept;
  const a = events.on('track-added',callback), b = events.on('track-added',callback), c = events.on('track-added',other);
  await events.off('track-added',callback);
  await flush(); assert.equal(t.messages.length,2);
  t.reply(1,{result:{guids:[]}});
  const [stopA,stopB,stopC] = await Promise.all([a,b,c]);
  assert.equal(removed,0); assert.equal(kept,1);
  t.event('track-added',1,{guids:['track']}); assert.equal(removed,0); assert.equal(kept,2);
  await stopA(); await stopB();
  const off = events.off('track-added',other);
  await answer(t,'ReaWeb_Unsubscribe',['track-added'],true,off);
  await stopC(); await events.off('track-added',other);
  const fresh = events.on('track-added',callback);
  await answer(t,'ReaWeb_Subscribe',['track-added'],{guids:[]},fresh);
  await stopA(); t.event('track-added',2,{guids:[]}); assert.equal(removed,2);
  await assert.rejects(events.off('not-an-event',callback),{code:'UNKNOWN_EVENT'});
  await assert.rejects(events.off('track-added',null),{code:'INVALID_ARGUMENT'});
});

test('off before the only subscription finishes suppresses its initial callback; native drops never replay', async () => {
  const t = await connected(), api = t.window.reaper;
  let calls = 0; const callback = () => ++calls;
  const registration = api.events.on('track-added',callback);
  const off = api.events.off('track-added',callback);
  await flush();
  assert.deepEqual(t.messages.slice(1).map(m=>m.method),['ReaWeb_Subscribe','ReaWeb_Unsubscribe']);
  t.reply(1,{result:{guids:[]}}); t.reply(2,{result:true}); await registration; await off;
  assert.equal(calls,0);
  const first = api.events.on('native-drop',callback);
  await answer(t,'ReaWeb_Subscribe',['native-drop'],{files:['should-not-replay']},first);
  assert.equal(calls,0);
  t.event('native-drop',1,{files:['sample.wav'],text:'',x:10,y:20}); assert.equal(calls,1);
  const second = await api.events.on('native-drop',callback); assert.equal(calls,1);
  t.event('native-drop',2,{files:[],text:'text',x:0,y:0}); assert.equal(calls,3);
  await answer(t,'ReaWeb_Unsubscribe',['native-drop'],true,api.events.off('native-drop',callback));
  await second(); t.event('native-drop',3,{files:[],text:'ignored',x:0,y:0}); assert.equal(calls,3);
});
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
  t.reply(0, { result: { protocol: 1, projectEpoch: 1, methods, batchMethods } });
  await t.window.reaper.lifecycle.ready;
  return t;
}
for (const engine of ['windows', 'webkit']) {
  test(`${engine}: builder preserves Mirror names and sends one batch with automatic result paths`, async () => {
    const t = await connected(engine);
    const pending = t.window.reaper.transaction.batch(b => {
      assert.deepEqual(Object.keys(b).sort(), batchMethods.slice().sort());
      const track = b.GetTrack(0, 0);
      const tuple = b.GetTrackName(track);
      const [, name] = tuple;
      const volume = b.GetMediaTrackInfo_Value(track, 'D_VOL');
      b.GetSetMediaTrackInfo_String(track, 'P_NAME', name, true);
      const changed = b.SetMediaTrackInfo_Value(track, 'D_VOL', volume);
      return { name, volume, nested: [tuple, { track, changed }], literal: 'snapshot' };
    }, { undoLabel: 'Track snapshot' });
    await flush();
    assert.equal(t.messages.length, 2);
    assert.equal(t.messages[1].method, 'ReaWeb_Batch');
    assert.deepEqual(t.messages[1].args, [[
      { method: 'GetTrack', args: [0, 0] },
      { method: 'GetTrackName', args: [{ $ref: 0 }] },
      { method: 'GetMediaTrackInfo_Value', args: [{ $ref: 0 }, 'D_VOL'] },
      { method: 'GetSetMediaTrackInfo_String', args: [{ $ref: 0 }, 'P_NAME', { $ref: 1, path: [1] }, true] },
      { method: 'SetMediaTrackInfo_Value', args: [{ $ref: 0 }, 'D_VOL', { $ref: 2 }] }
    ], { undoLabel: 'Track snapshot' }]);
    const track = { type: 'MediaTrack', id: 'track' };
    t.reply(1, { result: [track, [false, ''], 0, [true, ''], true] });
    assert.deepEqual(JSON.parse(JSON.stringify(await pending)), {
      name: '', volume: 0, nested: [[false, ''], { track, changed: true }], literal: 'snapshot'
    });
  });
}

test('builder gates collection on the handshake and returns raw, scalar, tuple and null results', async () => {
  const t = setup();
  let ran = false;
  const raw = t.window.reaper.transaction.batch(b => { ran = true; b.CountTracks(0); b.UpdateArrange(); });
  assert.equal(ran, false);
  t.reply(0, { result: { protocol: 1, projectEpoch: 1, methods, batchMethods } });
  await flush(); t.reply(1, { result: [2, null] });
  assert.deepEqual(Array.from(await raw), [2, null]);
  for (const [callback, result] of [
    [b => b.CountTracks(0), 0], [b => b.GetTrack(0, 99), null], [b => b.UpdateArrange(), null],
    [b => b.CountProjectMarkers(0), [3, 1, 2]], [b => { b.CountTracks(0); return false; }, false]
  ]) {
    const pending = t.window.reaper.transaction.batch(callback);
    await flush(); t.reply(t.messages.length - 1, { result: [result] });
    assert.deepEqual(JSON.parse(JSON.stringify(await pending)), result);
  }
});

test('builder preserves optional argument holes and binary tuple references', async () => {
  const t = await connected(), take = { type: 'MediaItem_Take', id: 'take' };
  const pending = t.window.reaper.transaction.batch(b => {
    const [ok, bytes] = b.MIDI_GetAllEvts(take);
    b.MIDI_SetAllEvts(take, bytes);
    b.MIDI_SetNote(take, 0, null, undefined, null, null, null, 60);
    return { ok, bytes };
  });
  await flush();
  assert.deepEqual(t.messages[1].args[0][1].args, [take, { $ref: 0, path: [1] }]);
  assert.deepEqual(t.messages[1].args[0][2].args, [take, 0, null, null, null, null, null, 60]);
  t.reply(1, { result: [[true, { __reawebBytes: 'AID/' }], true, true] });
  const result = await pending;
  assert.equal(result.ok, true);
  assert.deepEqual(result.bytes, new Uint8Array([0, 128, 255]));
});

test('builder rejects unsupported APIs, async callbacks, coercion and invalid structures before dispatch', async () => {
  const t = await connected(), batch = t.window.reaper.transaction.batch;
  for (const callback of [
    b => b.Main_OnCommand(40004, 0), b => b.GetUserInputs('title', 1, 'value', ''),
    b => b.window.close(), b => b.Typo(), () => {},
    async b => { await b.CountTracks(0); },
    async b => { b.CountTracks(0); }, b => { b.CountTracks(0); return Promise.resolve(1); },
    b => { b.CountTracks(0); return { pending: Promise.reject(new Error('not synchronous')) }; },
    b => +b.CountTracks(0), b => `${b.CountTracks(0)}`,
    b => b.GetTrackName({ wrapped: b.GetTrack(0, 0) }), b => b.GetTrackName({ $ref: 0 }),
    b => { b.CountTracks(0); const cycle = {}; cycle.self = cycle; return cycle; },
    b => { b.CountTracks(0); return new Date(); },
    b => { for (let i = 0; i < 129; i++) b.CountTracks(0); }
  ]) await assert.rejects(batch(callback), { code: 'INVALID_ARGUMENT' });
  const original = new Error('callback failed');
  await assert.rejects(batch(b => { b.CountTracks(0); throw original; }), error => error === original);
  await flush();
  assert.equal(t.messages.length, 1);
  assert.equal(t.timers.size, 0);
  const maximum = batch(b => { for (let i = 0; i < 128; i++) b.CountTracks(0); });
  await flush(); assert.equal(t.messages[1].args[0].length, 128);
  t.reply(1, { result: Array(128).fill(1) });
  assert.equal((await maximum).length, 128);
});

test('builder snapshots return structures and refuses escaped or cross-builder references', async () => {
  const t = await connected(), batch = t.window.reaper.transaction.batch;
  let ref, saved, output, method;
  const first = batch(b => {
    saved = b; method = b.CountTracks; ref = b.CountTracks(0);
    output = { nested: [ref] }; return output;
  });
  await flush(); output.nested[0] = 'changed';
  assert.throws(() => saved.CountTracks(0), { code: 'INVALID_ARGUMENT' });
  assert.throws(() => method(0), { code: 'INVALID_ARGUMENT' });
  await assert.rejects(batch(b => b.GetTrack(0, ref)), { code: 'INVALID_ARGUMENT' });
  await assert.rejects(batch(b => { b.CountTracks(0); return { ref }; }), { code: 'INVALID_ARGUMENT' });
  await assert.rejects(t.window.reaper.GetTrack(0, ref), { code: 'INVALID_ARGUMENT' });
  t.reply(1, { result: [7] });
  assert.deepEqual(JSON.parse(JSON.stringify(await first)), { nested: [7] });
  assert.equal(t.messages.length, 2);
});

test('builder preserves native error codes and partial results without projecting them', async () => {
  const t = await connected();
  for (const code of ['BATCH_FAILED', 'API_UNAVAILABLE', 'UNDO_BUSY', 'PROJECT_CHANGED']) {
    const details = { completed: 1, results: [null], rolledBack: false, cause: 'STALE_HANDLE' };
    const rejected = assert.rejects(t.window.reaper.transaction.batch(b => {
      b.UpdateArrange(); return { count: b.CountTracks(0) };
    }), error => error.code === code && JSON.stringify(error.details) === JSON.stringify(details));
    await flush();
    t.reply(t.messages.length - 1, { error: { code, message: 'Native failure', details } });
    await rejected;
  }
});

test('builder uses the advertised whitelist and reports missing capabilities or a closed document', async () => {
  for (const advertised of [undefined, ['CountTracks']]) {
    const t = setup();
    t.reply(0, { result: { protocol: 1, projectEpoch: 1, methods, batchMethods: advertised } });
    await assert.rejects(t.window.reaper.transaction.batch(b => b.GetTrack(0, 0)),
      { code: advertised ? 'INVALID_ARGUMENT' : 'SCHEMA_MISMATCH' });
    assert.equal(t.messages.length, 1);
  }
  const t = await connected();
  t.listeners.pagehide();
  let ran = false;
  await assert.rejects(t.window.reaper.transaction.batch(() => { ran = true; }), { code: 'WINDOW_CLOSED' });
  assert.equal(ran, false);
});
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
  const off1 = t.window.reaper.events.on('windowstatechange', s => states.push(s.docked));
  const off2 = t.window.reaper.events.on('windowstatechange', s => more.push(s.docked));
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
  const first = t.window.reaper.audio.setTrackValueLatest(track, 'D_VOL', .1);
  const middle = t.window.reaper.audio.setTrackValueLatest(track, 'D_VOL', .2);
  const last = t.window.reaper.audio.setTrackValueLatest(track, 'D_VOL', .3);
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
  await assert.rejects(t.window.reaper.window.open('x'.repeat(64*1024*1024)), { code: 'MESSAGE_LIMIT' });
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
  const value = t.window.reaper.transaction.withUndo('Edit', async () => 42);
  await flush(); assert.equal(t.messages[1].method, 'ReaWeb_BeginUndo');
  t.reply(1, {result: 'token'}); await flush();
  assert.equal(t.messages[2].method, 'ReaWeb_EndUndo');
  assert.equal(t.messages[2].args[0], 'token');
  t.reply(2, {result: true}); assert.equal(await value, 42);
  const failure = assert.rejects(t.window.reaper.transaction.withUndo('Fail', async () => { throw new Error('original'); }), /original/);
  await flush(); t.reply(3, {result: 'second'}); await flush();
  t.reply(4, {error: {code:'STALE_UNDO', message:'closed'}});
  await failure;
});

test('host I/O supports large text', async () => {
  const t = await connected();
  const writing = t.window.reaper.fs.writeFile('large.txt', 'x'.repeat(100000));
  await flush(); assert.equal(t.messages[1].args[1].length, 100000);
  t.reply(1, {result:{bytes:100000}}); assert.equal((await writing).bytes, 100000);
});

for (const name of ['itemselectionchange', 'takeselectionchange', 'transportchange', 'fxchange']) {
  test(name + ': subscribes and releases its native observer', async () => {
    const t = await connected(), received = [];
    const subscription = t.window.reaper.events.on(name, state => received.push(state));
    await flush(); t.reply(1, {result:null}); const dispose = await subscription;
    t.event(name, 1, {revision:1}); assert.equal(received[0].revision, 1);
    const closed = dispose(); await flush(); assert.equal(t.messages[2].method, 'ReaWeb_Unsubscribe');
    t.reply(2, {result:true}); await closed;
  });
}

test('Runtime namespaces preserve bridge arguments and cancellation', async () => {
  const t = await connected();
  const api = t.window.reaper;
  for (const name of ['window', 'dialog', 'events', 'theme', 'debug', 'audio', 'lifecycle']) assert.ok(Object.isFrozen(api[name]));
  const bounds = {x:10,y:20,width:800,height:600,mode:'floating',units:'native'};
  const size = await answer(t, 'ReaWeb_GetBounds', [], bounds, api.window.getSize());
  assert.equal(size.width, 800);
  await answer(t, 'ReaWeb_SetBounds', [{width:900,height:700}], bounds, api.window.setSize(900,700));
  await answer(t, 'ReaWeb_SetVisible', [false], {visible:false}, api.window.hide());
  assert.equal(await answer(t, 'GetUserFileName', [1,'Audio','','Audio|*.wav;*.aiff'], [false,''], api.dialog.openFile({title:'Audio',filters:[{name:'Audio',extensions:['wav','aiff']}]})), null);
  assert.equal(await answer(t, 'GetUserFileName', [0,'','mix.wav',''], [true,'/音频/mix.wav'], api.dialog.saveFile({initialPath:'mix.wav'})), '/音频/mix.wav');
  await answer(t, 'GetUserFileName', [3,'','',''], [true,'/音频'], api.dialog.selectFolder());
  await assert.rejects(api.dialog.openFile({filters:[{name:'bad|filter',extensions:['wav']}]}), {code:'INVALID_ARGUMENT'});
  await answer(t, 'ReaWeb_AudioFileInfo', ['a.wav'], {sampleRate:48000}, api.audio.getFileInfo('a.wav'));
  await answer(t, 'ReaWeb_AudioWaveform', ['a.wav',{points:512}], {points:512,data:[]}, api.audio.getWaveform('a.wav',{points:512}));
  const track = {type:'MediaTrack',id:'1:1'};
  await answer(t, 'ReaWeb_GetTrackMeter', [track], {channels:2,peak:[0.5,0],peakDb:[-6.02,null]}, api.audio.getTrackMeter(track));
});

test('Lifecycle waits for async cleanup, handles duplicate notifications and releases listeners', async () => {
  const t = await connected();
  await assert.rejects(t.window.reaper.lifecycle.on('destroy', () => {}), {code:'INVALID_ARGUMENT'});
  let release, cleanups = 0;
  const gate = new Promise(resolve => { release = resolve; });
  const stop = await answer(t, 'ReaWeb_LifecycleSubscribe', [true], true,
    t.window.reaper.lifecycle.on('before-close', async event => { assert.equal(event.reason, 'close'); await gate; }));
  await answer(t, 'ReaWeb_LifecycleSubscribe', [true], true,
    t.window.reaper.lifecycle.on('cleanup', () => { ++cleanups; }));
  const message = {document:t.messages[0].document,lifecycle:{event:'before-close',reason:'close',token:'one',timeoutMs:2000}};
  t.window.__reawebReceive(message); t.window.__reawebReceive(message);
  await flush(); assert.equal(cleanups,1); assert.equal(t.messages.length,3);
  release(); await flush();
  assert.equal(t.messages[3].method,'ReaWeb_LifecycleComplete');
  assert.deepEqual(t.messages[3].args,['one']);
  t.reply(3,{result:true}); await flush(); assert.equal(t.timers.size,0);
  await stop();
});

test('window.open preserves Promise window IDs and host errors without flat aliases', async () => {
  const t = await connected();
  assert.equal(Object.hasOwn(t.window.reaper, 'ReaWebOpen'), false);
  assert.equal(Object.hasOwn(t.window.reaper, 'ReaWeb_Open'), false);
  assert.equal(await answer(t, 'ReaWeb_Open', ['Other/index.html'], 7, t.window.reaper.window.open('Other/index.html')), 7);
  const rejected = assert.rejects(t.window.reaper.window.open('missing.html'), {code:'FILE_NOT_FOUND'});
  await flush();
  t.reply(t.messages.length - 1, {error:{code:'FILE_NOT_FOUND',message:'HTML file does not exist'}});
  await rejected;
});

test('Lifecycle timeout does not let an unresolved callback prevent native completion', async () => {
  const t = await connected();
  await answer(t, 'ReaWeb_LifecycleSubscribe', [true], true,
    t.window.reaper.lifecycle.on('before-reload', () => new Promise(() => {})));
  t.window.__reawebReceive({document:t.messages[0].document,lifecycle:{event:'before-reload',reason:'reload',token:'timeout',timeoutMs:2000}});
  for (const timer of [...t.timers.values()]) timer();
  await flush(); assert.equal(t.messages.at(-1).method,'ReaWeb_LifecycleComplete');
  t.reply(t.messages.length-1,{result:true}); await flush(); assert.equal(t.timers.size,0);
});

test('Theme following restores prior CSS values and runtime events remain typed subscriptions', async () => {
  const t = await connected();
  const values = new Map([['--reaper-text','red']]);
  const style = {getPropertyValue:n=>values.get(n)||'',getPropertyPriority:()=>'',setProperty:(n,v)=>values.set(n,v),removeProperty:n=>values.delete(n)};
  const theme = {available:true,cssVariables:{'--reaper-background':'#123456','--reaper-text':'#eeeeee'}};
  const stop = await answer(t,'ReaWeb_Subscribe',['theme-changed'],theme,t.window.reaper.theme.apply({style}));
  assert.equal(values.get('--reaper-background'),'#123456');
  await answer(t,'ReaWeb_Unsubscribe',['theme-changed'],true,stop());
  assert.equal(values.get('--reaper-text'),'red'); assert.ok(!values.has('--reaper-background'));
  const seen=[];
  const off = await answer(t,'ReaWeb_Subscribe',['track-added'],null,t.window.reaper.events.on('track-added',e=>seen.push(e)));
  t.event('track-added',1,{projectEpoch:1,revision:2,guids:['a']});
  assert.equal(seen[0].guids[0],'a');
  await answer(t,'ReaWeb_Unsubscribe',['track-added'],true,off());
});

test('Debug captures JS errors and bounded circular object previews', async () => {
  const t = await connected();
  const object={name:'test'}; object.self=object;
  const logged=t.window.reaper.debug.inspect(object); await flush();
  assert.equal(t.messages.at(-1).method,'ReaWeb_Log');
  assert.match(t.messages.at(-1).args[0].message,/Circular/);
  t.reply(t.messages.length-1,{result:true}); await logged;
  t.listeners.error({message:'script failed'}); await flush();
  assert.equal(t.messages.at(-1).args[0].level,'error');
  assert.equal(t.messages.at(-1).args[0].message,'script failed');
  t.reply(t.messages.length-1,{result:true}); await flush();
});
