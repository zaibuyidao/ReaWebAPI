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
// The host owns the event list and advertises it in the handshake. A second
// hardcoded copy in the browser used to drift from it, and the mismatch only
// surfaced as "UNKNOWN_EVENT" in the page at runtime.
const hostEvents = [...fs.readFileSync(path.join(__dirname, '../src/runtime/services.cpp'), 'utf8')
  .match(/static const std::vector<std::string> names = \{([\s\S]*?)\};/)[1]
  .matchAll(/"([^"]+)"/g)].map(match => match[1]);
const bridgeSource = fs.readFileSync(path.join(__dirname, '../runtime/reaper.js'), 'utf8');

test('the browser holds no event list of its own', () => {
  assert.ok(hostEvents.length > 30, 'the host event list was not parsed');
  // A literal list here is the defect: it silently disagrees with the host.
  assert.equal(/const eventNames = \[/.test(bridgeSource), false,
    'runtime/reaper.js must not hardcode event names; read capabilities.events instead');
  assert.match(bridgeSource, /capabilities\.events\.includes\(name\)/);
});

test('every event the host advertises is subscribable from the page', async () => {
  const t = setup();
  // Advertise the host's real list, then subscribe to every name in it.
  t.reply(0, { result: { protocol: 1, projectEpoch: 1, methods, batchMethods, events: hostEvents } });
  await t.window.reaper.lifecycle.ready;
  for (const name of hostEvents) {
    const before = t.messages.length;
    // on() awaits the subscribe reply, so answer it before awaiting on().
    const pending = t.window.reaper.events.on(name, () => {});
    await flush();
    const subscribe = t.messages.slice(before).find(message => message.method === 'ReaWeb_Subscribe');
    assert.ok(subscribe, `no ReaWeb_Subscribe was sent for "${name}"`);
    assert.deepEqual(subscribe.args, [name]);
    t.reply(t.messages.indexOf(subscribe), { result: null });
    const dispose = await pending;
    // dispose() awaits the unsubscribe reply, so answer that one the same way.
    const closing = dispose();
    await flush();
    const unsubscribe = t.messages.slice(before).find(message => message.method === 'ReaWeb_Unsubscribe');
    assert.ok(unsubscribe, `no ReaWeb_Unsubscribe was sent for "${name}"`);
    assert.deepEqual(unsubscribe.args, [name]);
    t.reply(t.messages.indexOf(unsubscribe), { result: true });
    await closing;
  }
  // A frame pushed by the host must reach a page listener with its payload intact.
  const frames = [];
  const framePending = t.window.reaper.events.on('frame', frame => frames.push(frame));
  await flush();
  const frameSubscribe = t.messages.slice(-1)[0];
  assert.equal(frameSubscribe.method, 'ReaWeb_Subscribe');
  assert.deepEqual(frameSubscribe.args, ['frame']);
  t.reply(t.messages.indexOf(frameSubscribe), { result: null });
  await framePending;
  t.event('frame', 1, { sequence: 7, playing: 1, tempo: 120, lua: '{"track":null}' });
  await flush();
  assert.deepEqual(frames, [{ sequence: 7, playing: 1, tempo: 120, lua: '{"track":null}' }]);
  // A name the host did not advertise is still rejected, so the check can fail.
  await assert.rejects(t.window.reaper.events.on('not-an-event', () => {}), { code: 'UNKNOWN_EVENT' });
  await assert.rejects(t.window.reaper.events.off('not-an-event', () => {}), { code: 'UNKNOWN_EVENT' });
  // A host that advertises nothing is a schema mismatch, not an empty allow list.
  const silent = setup();
  silent.reply(0, { result: { protocol: 1, projectEpoch: 1, methods, batchMethods } });
  await silent.window.reaper.lifecycle.ready;
  await assert.rejects(silent.window.reaper.events.on('message', () => {}), { code: 'SCHEMA_MISMATCH' });
});

test('The public SDK has exactly 730 unchanged Mirror methods and seventeen frozen Runtime namespaces', async () => {
  const t = await connected();
  const api = t.window.reaper;
  const expected = {
  "host": ["send", "service"],
  "stream": ["open", "getDiagnostics"],
  "video": ["getCapabilities", "getSourceInfo", "openStream", "openWorkerStream", "openSurface", "getState", "getDiagnostics"],
  "realtime": ["subscribe", "getCapabilities", "getDiagnostics"],
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
    "setFrameless",
    "setCornerRadius",
    "isDocked",
    "setKeyboardCapture",
    "setMouseMode",
    "setMouseRegions",
    "setDawKeyboard",
    "setShortcutRouting",
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
    "openMedia",
    "openWrite",
    "watch",
    "readText",
    "writeText",
    "readBinary",
    "writeBinary",
    "readFile",
    "writeFile",
    "stat",
    "readDirectory",
    "makeDirectory",
    "moveFile"
  ],
  "audio": [
    "openStream",
    "resetMeter",
    "decodeMeter",
    "getFileInfo",
    "getWaveform",
    "getTrackMeter",
    "setTrackValueLatest"
  ],
  "clipboard": [
    "readBinary", "writeBinary",
    "readText",
    "writeText"
  ],
  "dragDrop": ["startFiles", "startText"],
  "app": ["getId", "getName", "getVersion", "getRootPath", "getDataPath"],
  "system": ["getDevices", "getDisplays", "openMIDIInput", "schedule", "getPlatform", "getArchitecture", "revealInFileManager",
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

test('Media URL handles preserve descriptors and release idempotently', async () => {
  const t = await connected(), io = t.window.reaper.fs;
  const descriptor = {url:'reaweb://app/__reaweb_media__/token',size:20000000,type:'audio/wav'};
  const media = await answer(t, 'ReaWeb_OpenMedia', ['outside.wav'], descriptor, io.openMedia('outside.wav'));
  assert.equal(media.url, descriptor.url);
  assert.equal(media.size, descriptor.size);
  assert.ok(Object.isFrozen(media));
  const release = media.close();
  assert.equal(media.close(), release);
  assert.equal(await answer(t, 'ReaWeb_CloseMedia', [descriptor.url], true, release), true);
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
    ['window','setMouseMode','ReaWeb_SetMouseMode',['regions'],true],
    ['window','setMouseRegions','ReaWeb_SetMouseRegions',[[{x:1,y:2,width:3,height:4}]],true],
    ['window','setFrameless','ReaWeb_SetFrameless',[false],{frameless:false}],
    ['window','setFrameless','ReaWeb_SetFrameless',[true],{frameless:true}],
    ['window','setCornerRadius','ReaWeb_SetCornerRadius',[24.5],{cornerRadius:24.5}],
    ['fs','moveFile','ReaWeb_MoveFile',['input.wav','output.wav',{overwrite:true}],{source:'input.wav',path:'output.wav',bytes:4}],
    ['window','isDocked','ReaWeb_IsDocked',[],false],
    ['window','setKeyboardCapture','ReaWeb_SetKeyboardCapture',[false],false],
    ['window','setDawKeyboard','ReaWeb_SetDawKeyboard',[true],true],
    ['window','setShortcutRouting','ReaWeb_SetShortcutRouting',[true],true],
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

test('a frame never replays to a listener that subscribes later', async () => {
  const t = await connected(), events = t.window.reaper.events;
  const seen = [];
  await answer(t, 'ReaWeb_Subscribe', ['frame'], null, events.on('frame', frame => seen.push(['first', frame.sequence])));
  t.event('frame', 1, { sequence: 7, playing: 1 }); await flush();
  assert.deepEqual(seen, [['first', 7]]);
  // The second listener joins while that frame is still the newest one. Every pass
  // replaces the frame, so handing it the previous one would show stale numbers.
  await events.on('frame', frame => seen.push(['second', frame.sequence]));
  await flush();
  assert.deepEqual(seen, [['first', 7]]);
  t.event('frame', 2, { sequence: 8, playing: 0 }); await flush();
  assert.deepEqual(seen, [['first', 7], ['first', 8], ['second', 8]]);
});
function setup(engine = 'windows', document) {
  const messages = [], reports = [], timers = new Map(), listeners = {}, releasedBuffers = [], nativeListeners = {};
  let timerId = 0;
  const post = message => message.startsWith('__reawebDiagnostic:')
    ? reports.push(JSON.parse(message.slice(19))) : messages.push(JSON.parse(message));
  const window = { addEventListener: (event, handler) => { listeners[event] = handler; } };
  window.top = window;
  if (engine === 'windows' || engine === 'windows-shared') window.chrome = { webview: { postMessage: post } };
  else if (engine === 'webkit') window.webkit = { messageHandlers: { reaweb: { postMessage: post } } };
  if (engine === 'windows-shared') Object.assign(window.chrome.webview, {
    addEventListener: (name, handler) => { nativeListeners[name] = handler; },
    releaseBuffer: buffer => releasedBuffers.push(buffer)
  });
  const context = vm.createContext({ window, document, crypto: webcrypto, TextEncoder, console, Uint8Array, Float32Array, Float64Array, DataView, btoa, atob,
    setTimeout: fn => { timers.set(++timerId, fn); return timerId; }, clearTimeout: id => timers.delete(id) });
  vm.runInContext(script, context);
  const reply = (index, payload) => window.__reawebReceive({ id: messages[index].id, document: messages[index].document, ...payload });
  const event = (name, sequence, data) => window.__reawebReceive({ document: messages[0].document, event: name, sequence, data });
  return { window, messages, reports, timers, listeners, reply, event, context, releasedBuffers, nativeListeners };
}
async function connected(engine) {
  const t = setup(engine);
  // The real handshake dispatches ReaWeb_GetCapabilities, which advertises events.
  t.reply(0, { result: { protocol: 1, projectEpoch: 1, methods, batchMethods, events: hostEvents } });
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

for (const engine of ['windows', 'webkit']) {
  test(`${engine}: transport queries and tuple conversions share one batch`, async () => {
    const t = await connected(engine);
    const pending = t.window.reaper.transaction.batch(b => {
      const position = b.GetPlayPosition();
      const state = b.GetPlayState();
      const [beats, measures] = b.TimeMap2_timeToBeats(0, position);
      const display = b.format_timestr_pos(position, '', 0);
      const valid = b.ValidatePtr2(0, null, 'MediaTrack*');
      return { position, state, beats, measures, display, valid };
    });
    await flush();
    assert.equal(t.messages.length, 2);
    assert.equal(t.messages[1].method, 'ReaWeb_Batch');
    assert.deepEqual(t.messages[1].args, [[
      { method: 'GetPlayPosition', args: [] },
      { method: 'GetPlayState', args: [] },
      { method: 'TimeMap2_timeToBeats', args: [0, { $ref: 0 }] },
      { method: 'format_timestr_pos', args: [{ $ref: 0 }, '', 0] },
      { method: 'ValidatePtr2', args: [0, null, 'MediaTrack*'] }
    ]]);
    t.reply(1, { result: [0, 0, [0, 0, 4, 0, 4], '0:00.000', false] });
    assert.deepEqual(JSON.parse(JSON.stringify(await pending)), {
      position: 0, state: 0, beats: 0, measures: 0, display: '0:00.000', valid: false
    });
  });
}
for (const engine of ['windows', 'webkit']) {
  test(`${engine}: named services preserve Lua routing, listeners, unload and pending cleanup`, async () => {
    const t = await connected(engine), api = t.window.reaper, testService = api.host.service('test');
    assert.ok(Object.isFrozen(testService));
    assert.throws(() => api.host.service('bad/name'), {code:'INVALID_ARGUMENT'});
    assert.equal(await answer(t, 'ReaWeb_ServiceInvoke', ['test','ping',null], 'pong', testService.invoke('ping')), 'pong');
    const jsonResult = {__reawebBytes:'business JSON',__reawebCall:true};
    assert.deepEqual(await answer(t,'ReaWeb_ServiceInvoke',['test','json',null],jsonResult,testService.invoke('json')),jsonResult);
    await answer(t, 'ReaWeb_HostSend', ['{"method":"old","payload":1}'], true, api.host.send({method:'old',payload:1}));
    let calls = 0, unloads = 0;
    const cb = () => ++calls;
    const pendingA = testService.on('changed',cb), pendingB = testService.on('changed',cb);
    await answer(t,'ReaWeb_ServiceSubscribe',['test','changed'],{handle:'1'},pendingA);
    const stopA = await pendingA, stopB = await pendingB;
    const event = (name, handle='1') => t.window.__reawebReceive({document:t.messages[0].document,service:'test',serviceHandle:handle,serviceEvent:name,data:{x:1}});
    event('changed'); assert.equal(calls,2);
    await stopA(); event('changed'); assert.equal(calls,3);
    assert.equal(testService.send('message',{x:1}),undefined);
    await answer(t,'ReaWeb_ServiceSend',['test','message',{x:1}],true);
    await answer(t,'ReaWeb_ServiceUnsubscribe',['test','changed'],{handle:'1'},testService.off('changed',cb));
    await stopB(); event('changed'); assert.equal(calls,3);
    const stopUnload = await answer(t,'ReaWeb_ServiceSubscribe',['test','unloaded'],{handle:'1'},testService.on('unloaded',()=>++unloads));
    const stopOld = await answer(t,'ReaWeb_ServiceSubscribe',['test','changed'],{handle:'1'},testService.on('changed',cb));
    event('unloaded'); assert.equal(unloads,1); event('changed'); assert.equal(calls,3);
    await stopOld(); await stopUnload();
    const stopNew = await answer(t,'ReaWeb_ServiceSubscribe',['test','changed'],{handle:'2'},testService.on('changed',cb));
    event('changed'); assert.equal(calls,3); event('changed','2'); assert.equal(calls,4);
    await stopOld(); event('changed','2'); assert.equal(calls,5);
    await answer(t,'ReaWeb_ServiceUnsubscribe',['test','changed'],{handle:'2'},stopNew());
    for (const code of ['SERVICE_NOT_FOUND','METHOD_NOT_FOUND','INVALID_ARGUMENT','EXTENSION_UNLOADED','MAIN_THREAD_REQUIRED','TIMEOUT']) {
      const check = assert.rejects(testService.invoke('ping'),{code});
      await flush(); t.reply(t.messages.length-1,{error:{code,message:code}}); await check;
    }
    const closing = assert.rejects(testService.invoke('pending'),{code:'WINDOW_CLOSED'});
    await flush(); t.listeners.pagehide(); await closing;
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
test('host messages serialize explicitly and reject invalid values before dispatch', async () => {
  const t = await connected();
  for (const value of ['鼓组 🎛️', '{"text":true}', '', {type:'setVolume',value:0.5}, [1,null,true], false, 0, null]) {
    const text = typeof value === 'string' ? value : JSON.stringify(value);
    assert.equal(await answer(t, 'ReaWeb_HostSend', [text], true, t.window.reaper.host.send(value)), true);
  }
  const circular = {}; circular.self = circular;
  for (const value of [undefined, () => {}, Symbol(), 1n, NaN, Infinity, {bad:undefined}, [undefined], circular, 'a\0b'])
    await assert.rejects(t.window.reaper.host.send(value), {code:'INVALID_ARGUMENT'});
  await assert.rejects(t.window.reaper.host.send('鼓'.repeat(350000)), {code:'MESSAGE_LIMIT'});
  const error = assert.rejects(t.window.reaper.host.send('full'), {code:'QUEUE_LIMIT'});
  await flush(); t.reply(t.messages.length - 1, {error:{code:'QUEUE_LIMIT',message:'Full'}}); await error;
});

test('message events preserve text and FIFO without snapshot replay or cross-document delivery', async () => {
  const t = await connected(), other = await connected(), seen = [], late = [];
  const pending = t.window.reaper.events.on('message', text => seen.push(text));
  await flush();
  t.event('message', 1, 'before subscription reply');
  t.reply(1, {result:null}); await pending;
  assert.deepEqual(seen, ['before subscription reply']);
  await t.window.reaper.events.on('message', text => late.push(text));
  assert.deepEqual(late, []);
  const values = ['A', '{"a":1}', '鼓 🎚', '', 'C'];
  values.forEach((text, i) => t.event('message', i + 2, text));
  assert.deepEqual(seen.slice(1), values); assert.deepEqual(late, values);
  const old = {document:'old',event:'message',sequence:100,data:'stale'};
  t.window.__reawebReceive(old); other.window.__reawebReceive({...old,document:t.messages[0].document});
  assert.deepEqual(seen.slice(1), values);
});

for (const engine of ['windows', 'webkit']) {
  test(`${engine}: replaceable frames and reliable events order independently`, async () => {
    const t = await connected(engine), messages = [], frames = [], states = [];
    for (const [name, seen] of [['message', messages], ['frame', frames], ['windowstatechange', states]]) {
      const pending = t.window.reaper.events.on(name, value => seen.push(value));
      await flush(); t.reply(t.messages.length - 1, {result:null}); await pending;
      seen.length = 0;
    }
    t.event('frame', 20, 'new frame');
    t.event('message', 19, 'queued message');
    t.event('windowstatechange', 21, 'queued state');
    t.event('frame', 18, 'obsolete worker frame');
    t.event('frame', 22, 'latest frame');
    t.event('message', 23, 'next message');
    t.event('message', 19, 'duplicate message');
    t.event('frame', 22, 'duplicate frame');
    t.event('frame', 22.5, 'invalid sequence');
    t.window.__reawebReceive({document:'old',event:'frame',sequence:100,data:'old document'});
    t.event('frame', 24, 'current document');
    t.event('windowstatechange', 30, 'new state');
    t.event('frame', 29, 'frame after newer reliable event');
    assert.deepEqual(messages, ['queued message', 'next message']);
    assert.deepEqual(states, ['queued state', 'new state']);
    assert.deepEqual(frames, ['new frame', 'latest frame', 'current document', 'frame after newer reliable event']);
  });
}

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
  assert.equal(await answer(t, 'ReaWeb_OpenEx', ['Other/index.html', {hidden:true}], 8, t.window.reaper.window.open('Other/index.html', {hidden:true})), 8);
  assert.equal(await answer(t, 'ReaWeb_OpenEx', ['Other/index.html', {hidden:false}], 9, t.window.reaper.window.open('Other/index.html', {hidden:false})), 9);
  assert.equal(await answer(t, 'ReaWeb_OpenEx', ['Other/index.html', {frameless:true,resizable:false}], 10, t.window.reaper.window.open('Other/index.html', {frameless:true,resizable:false})), 10);
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
const streamDescriptor = (kind = 'frame', name = 'test.frames') => ({ name, kind, format: kind === 'audio' ? 'float32' : 'rgba8', capacity: 3, maxBytes: 16, width: 2, height: 2, stride: 8, token: 'ticket', url: 'ws://127.0.0.1:9000/ticket' });
test('Built-in meter reset forwards its owned stream name and preserves host errors', async () => {
  const t = await connected();
  assert.equal(await answer(t, 'ReaWeb_MeterReset', ['runtime.audio.1'], true, t.window.reaper.audio.resetMeter('runtime.audio.1')), true);
  const rejected = assert.rejects(t.window.reaper.audio.resetMeter('third.party'), { code: 'INVALID_HANDLE' });
  await flush(); t.reply(t.messages.length - 1, { error: { code: 'INVALID_HANDLE', message: 'Expected owned built-in meter' } });
  await rejected;
});
function streamSockets(t) {
  const sockets = [];
  t.context.WebSocket = class {
    constructor(url) { this.url = url; this.sent = []; sockets.push(this); }
    send(bytes) { this.sent.push(bytes); }
    close() { this.onclose?.({ reason: 'STREAM_CLOSED' }); }
  };
  return sockets;
}
function streamPacket(sequence, kind = 1, bytes = new Uint8Array(16)) {
  const buffer = new ArrayBuffer(40 + bytes.byteLength), view = new DataView(buffer);
  view.setUint32(0, 0x01535752, true); view.setUint8(4, kind);
  view.setBigUint64(8, BigInt(sequence), true); view.setFloat64(16, sequence / 60, true);
  view.setBigUint64(24, 2n, true); view.setBigUint64(32, BigInt(bytes.byteLength), true);
  new Uint8Array(buffer, 40).set(bytes); return { data: buffer };
}
test('Native streams cache binary frames, ACK delivery and detach only one consumer', async () => {
  const t = await connected(), sockets = streamSockets(t), api = t.window.reaper;
  const pending = api.stream.open('test.frames'); await flush();
  assert.equal(t.messages.at(-1).method, 'ReaWeb_StreamOpen');
  t.reply(t.messages.length - 1, { result: streamDescriptor() }); await flush();
  const socket = sockets[0]; assert.equal(socket.binaryType, 'arraybuffer'); socket.onopen();
  const stream = await pending, received = [], ended = [];
  stream.on('data', value => received.push(value)); stream.on('close', error => ended.push(error.code));
  assert.equal(stream.latest(), null);
  const requests = t.messages.length;
  for (let i = 1; i <= 100; ++i) socket.onmessage(streamPacket(i));
  assert.equal(stream.latest().frameId, 100n); assert.equal(stream.latest().bytes.byteLength, 16);
  assert.equal(stream.latest().producerDropped, 2n); assert.equal(received.length, 100);
  assert.equal(t.messages.length, requests, 'latest and frame delivery never use RPC');
  assert.equal(socket.sent.length, 100); assert.equal(socket.sent[0][0], 1);
  assert.equal(stream.latest().discontinuity, undefined);
  const invalidation = streamPacket(101); new DataView(invalidation.data).setUint8(5, 1);
  socket.onmessage(invalidation);
  assert.equal(stream.latest().discontinuity, true);
  socket.onmessage(streamPacket(102));
  assert.equal(stream.latest().discontinuity, undefined, 'Normal frames must not retain an invalidation flag');
  const closing = stream.close(); await flush();
  assert.equal(t.messages.at(-1).method, 'ReaWeb_StreamDetach');
  t.reply(t.messages.length - 1, { result: true }); await closing;
  assert.equal(stream.closed, true); assert.equal(stream.latest(), null); assert.deepEqual(ended, ['STREAM_CLOSED']);
  await stream.close(); assert.equal(t.messages.length, requests + 1);
});
test('Shared video ACKs immutable snapshots and releases mappings on invalid slots or close', async () => {
  for (const early of [true, false]) {
    const t = await connected('windows-shared'), sockets = streamSockets(t), buffer = new ArrayBuffer(32);
    const deliver = () => t.nativeListeners.sharedbufferreceived({ additionalData: { reawebVideo: 'ticket' }, getBuffer: () => buffer });
    const pending = t.window.reaper.stream.open('reaweb.video.project.test'); await flush();
    if (early) deliver();
    t.reply(t.messages.length - 1, { result: { ...streamDescriptor(), sharedVideo: true } }); await flush();
    if (!early) { assert.equal(sockets.length, 0); deliver(); await flush(); }
    const socket = sockets[0]; socket.onopen(); const stream = await pending, frames = [];
    stream.on('data', frame => frames.push(frame));
    for (let i = 0; i < 5; ++i) {
      new Uint8Array(buffer, (i % 2) * 16, 16).fill(i);
      const message = streamPacket(i, 1, new Uint8Array(4)), view = new DataView(message.data);
      view.setUint8(5, 2); view.setBigUint64(32, 16n, true); view.setUint32(40, i % 2, true);
      socket.onmessage(message);
    }
    assert.equal(socket.sent.length, 5);
    frames.forEach((frame, i) => assert.deepEqual([...frame.bytes], Array(16).fill(i)));
    assert.equal(stream.info.sharedVideo, undefined);
    if (early) {
      const malformed = streamPacket(6, 1, new Uint8Array(4)), view = new DataView(malformed.data);
      view.setUint8(5, 2); view.setBigUint64(32, 16n, true); view.setUint32(40, 2, true);
      socket.onmessage(malformed); assert.equal(stream.error.code, 'UNSUPPORTED_FORMAT');
    } else {
      const close = stream.close(); await flush(); t.reply(t.messages.length - 1, { result: true }); await close;
    }
    assert.deepEqual(t.releasedBuffers, [buffer]);
  }
});

function dynamicVideoPacket(sequence, width, height, planar, shared, slot = 0) {
  const size = width * height * (planar ? 1.5 : 4), buffer = new ArrayBuffer(shared ? 60 : 56 + size), view = new DataView(buffer);
  view.setUint32(0, 0x03535752, true); view.setUint8(4, 1); view.setUint8(5, shared ? 2 : 0);
  view.setBigUint64(8, BigInt(sequence), true); view.setFloat64(16, sequence / 60, true); view.setBigUint64(32, BigInt(size), true);
  view.setUint32(40, width, true); view.setUint32(44, height, true); view.setUint32(48, width * (planar ? 1 : 4), true);
  if (shared) view.setUint32(56, slot, true); else new Uint8Array(buffer, 56).fill(sequence);
  return { data: buffer };
}
test('Dynamic video carries immutable per-frame dimensions through socket, shared memory, resources and native copies', async () => {
  for (const mode of ['raw', 'shared', 'resource', 'native']) for (const format of ['rgba8', 'i420']) {
    const t = await connected(mode === 'shared' ? 'windows-shared' : 'webkit'), sockets = streamSockets(t);
    const maxBytes = 64, mapping = new ArrayBuffer(maxBytes * 2), slots = new Uint8Array(mapping), frames = [];
    const planar = format === 'i420'; let currentSize = 0;
    t.context.ArrayBuffer = ArrayBuffer; t.context.AbortController = AbortController;
    t.context.fetch = async url => ({ ok: true, arrayBuffer: async () => slots.slice(Number(url.at(-1)) * maxBytes, Number(url.at(-1)) * maxBytes + currentSize).buffer });
    t.window.__reawebVideoCopy = (token, slot, size) => {
      if (slot === -1) return maxBytes;
      if (slot === -2) return null;
      assert.equal(size, currentSize);
      return slots.slice(slot * maxBytes, slot * maxBytes + size).buffer;
    };
    const opening = t.window.reaper.stream.open('reaweb.video.project.test'); await flush();
    if (mode === 'shared') t.nativeListeners.sharedbufferreceived({ additionalData: { reawebVideo: 'ticket' }, getBuffer: () => mapping });
    t.reply(t.messages.length - 1, { result: { ...streamDescriptor(), format, stride: planar ? 2 : 8, maxBytes,
      dynamicResolution: true, layoutVersion: 3, sharedVideo: mode !== 'raw',
      ...(mode === 'resource' ? { videoResource: 'reaweb://test/video/' } : {}),
      ...(mode === 'native' ? { videoNativeCopy: true } : {}) } });
    await flush(); sockets[0].onopen(); const stream = await opening;
    stream.on('data', frame => frames.push(frame));
    for (const [index, [width, height]] of [[2, 2], [4, 2], [2, 4], [2, 2]].entries()) {
      currentSize = width * height * (planar ? 1.5 : 4);
      slots.fill(index + 1, (index % 2) * maxBytes, (index % 2) * maxBytes + currentSize);
      sockets[0].onmessage(dynamicVideoPacket(index + 1, width, height, planar, mode !== 'raw', index % 2));
      await flush(); await flush();
      const frame = frames.at(-1);
      assert.deepEqual([frame.width, frame.height, frame.stride, frame.bytes.length], [width, height, width * (planar ? 1 : 4), currentSize]);
      assert.equal(sockets[0].sent.length, index + 1);
    }
    frames.forEach((frame, index) => assert.ok(frame.bytes.every(value => value === index + 1)));
    assert.equal(sockets.length, 1); assert.equal(stream.closed, false); assert.equal(stream.info.width, 2);
    const invalid = dynamicVideoPacket(5, 2, 2, planar, mode !== 'raw'); new DataView(invalid.data).setUint32(48, 1, true);
    sockets[0].onmessage(invalid); await flush();
    assert.equal(stream.error.code, 'UNSUPPORTED_FORMAT'); assert.equal(sockets[0].sent.length, 4);
  }
});

test('Dynamic I420 rejects dimensions exceeding the RGBA-equivalent pixel budget', async () => {
  const t = await connected('webkit'), sockets = streamSockets(t);
  const opening = t.window.reaper.stream.open('reaweb.video.project.test'); await flush();
  t.reply(t.messages.length - 1, { result: { ...streamDescriptor(), format: 'i420', stride: 2,
    maxBytes: 40 * 1024 * 1024, dynamicResolution: true, layoutVersion: 3 } });
  await flush(); sockets[0].onopen(); const stream = await opening;
  sockets[0].onmessage(dynamicVideoPacket(1, 4096, 3072, true, false));
  assert.equal(stream.error.code, 'UNSUPPORTED_FORMAT'); assert.equal(sockets[0].sent.length, 0);
});

async function workerVideo(shared = true, format = 'rgba8', dynamic = false) {
  const t = await connected('windows-shared');
  class Port {
    postMessage(data, transfer = []) {
      const message = structuredClone(data, { transfer });
      queueMicrotask(() => { if (!this.other.closed) this.other.onmessage?.({ data: message }); });
    }
    close() { this.closed = true; }
  }
  t.context.MessageChannel = class {
    constructor() { this.port1 = new Port(); this.port2 = new Port(); this.port1.other = this.port2; this.port2.other = this.port1; }
  };
  t.context.Worker = class {
    addEventListener(name, fn) { this[name] = fn; }
    removeEventListener(name) { delete this[name]; }
    terminate() { this.terminated = true; }
    postMessage(data) {
      this.port = data.port;
      this.buffer = data.buffer ? structuredClone(data.buffer, { transfer: [data.buffer] }) : null;
      this.port.onmessage = ({ data }) => {
        if (data.type === 'close' && !this.unresponsive) {
          this.port.postMessage({ type: 'closed', buffer: this.buffer }, this.buffer ? [this.buffer] : []);
          this.buffer = null;
        }
      };
      this.port.postMessage({ type: 'ready' });
    }
  };
  const worker = new t.context.Worker(), buffer = new ArrayBuffer(dynamic ? 128 : format === 'i420' ? 12 : 32);
  const pending = t.window.reaper.video.openWorkerStream(worker, { resolution: 'source', format }); await flush();
  assert.equal(t.messages.at(-1).method, 'ReaWeb_VideoOpen');
  t.reply(t.messages.length - 1, { result: 'reaweb.video.project.test' }); await flush();
  if (shared) t.nativeListeners.sharedbufferreceived({ additionalData: { reawebVideo: 'ticket' }, getBuffer: () => buffer });
  t.reply(t.messages.length - 1, { result: { ...streamDescriptor(), sharedVideo: shared, ...(format === 'i420' ? { format, stride: 2, maxBytes: 6, colorMatrix: 'bt709', colorRange: 'limited', colorConversion: 'reaper-sse' } : {}),
    ...(dynamic ? { dynamicResolution: true, layoutVersion: 3, maxBytes: 64 } : {}) } });
  const stream = await pending;
  return { t, worker, buffer, stream };
}
test('Worker video transfers native ownership once, returns it on close and preserves the regular stream contract', async () => {
  for (const shared of [true, false]) {
    const { t, worker, buffer, stream } = await workerVideo(shared);
    assert.equal(buffer.byteLength, shared ? 0 : 32);
    assert.equal(stream.info.token, undefined); assert.equal(stream.info.sharedVideo, undefined);
    const frames = []; stream.on('data', frame => frames.push(frame));
    worker.port.postMessage({ type: 'frame', frame: { sequence: 7n, timestamp: 1, producerDropped: 0n, bytes: new Uint8Array(16) } }); await flush();
    assert.equal(frames[0].sequence, 7n); assert.equal(frames[0].bytes, undefined); assert.ok(Object.isFrozen(frames[0]));
    await assert.rejects(t.window.reaper.video.openWorkerStream(worker), { code: 'INVALID_ARGUMENT' });
    const closing = stream.close(); await flush();
    assert.equal(t.releasedBuffers.length, shared ? 1 : 0);
    if (shared) assert.equal(t.releasedBuffers[0].byteLength, 32);
    assert.equal(t.messages.at(-1).method, 'ReaWeb_StreamDetach');
    t.reply(t.messages.length - 1, { result: true }); await closing;
    const count = t.messages.length; await stream.close(); assert.equal(t.messages.length, count);
    assert.ok(stream.closed); assert.ok(!worker.terminated);
  }
});
test('I420 worker connections preserve conversion metadata and planar memory ownership', async () => {
  for (const shared of [true, false]) {
    const { t, worker, stream } = await workerVideo(shared, 'i420');
    assert.equal(stream.info.colorConversion, 'reaper-sse'); assert.equal(stream.info.maxBytes, 6);
    assert.equal(worker.buffer?.byteLength || 0, shared ? 12 : 0);
    const closing = stream.close(); await flush();
    t.reply(t.messages.length - 1, { result: true }); await closing;
    assert.equal(t.releasedBuffers[0]?.byteLength || 0, shared ? 12 : 0);
  }
});
test('Worker video forwards changing frame dimensions and rejects malformed metadata', async () => {
  const { t, worker, stream } = await workerVideo(true, 'rgba8', true), frames = [];
  stream.on('data', frame => frames.push(frame));
  for (const [index, [width, height]] of [[2, 2], [4, 2], [2, 4]].entries()) {
    worker.port.postMessage({ type: 'frame', frame: { sequence: BigInt(index), timestamp: index / 60, producerDropped: 0n, width, height, stride: width * 4 } });
    await flush(); assert.deepEqual([frames.at(-1).width, frames.at(-1).height], [width, height]);
  }
  worker.port.postMessage({ type: 'frame', frame: { sequence: 4n, timestamp: 1, producerDropped: 0n, width: 4, height: 4, stride: 8 } });
  await flush(); assert.equal(stream.error.code, 'UNSUPPORTED_FORMAT'); assert.equal(frames.length, 3);
  t.reply(t.messages.length - 1, { result: true }); await stream.close();
});

test('Worker failures and unresponsive closure release native consumers without releasing detached buffers', async () => {
  for (const crash of [true, false]) {
    const { t, worker, stream } = await workerVideo();
    if (crash) worker.error({ message: 'worker crashed' });
    else { worker.unresponsive = true; void stream.close(); for (const fn of [...t.timers.values()]) fn(); }
    await flush();
    assert.equal(worker.terminated, true); assert.equal(t.releasedBuffers.length, 0);
    assert.equal(t.messages.at(-1).method, 'ReaWeb_StreamDetach');
    t.reply(t.messages.length - 1, { result: true }); await stream.close();
    assert.equal(stream.closed, true);
  }
});

test('Native I420 resources retain their complete planar payload and color metadata', async () => {
  const t = await connected('webkit'), sockets = streamSockets(t);
  t.context.AbortController = AbortController;
  t.context.fetch = async () => ({ ok: true, arrayBuffer: async () => new Uint8Array(24).fill(128).buffer });
  const opening = t.window.reaper.video.openStream({ resolution: 'source', format: 'i420' }); await flush();
  t.reply(t.messages.length - 1, { result: 'reaweb.video.project.test' }); await flush();
  t.reply(t.messages.length - 1, { result: { ...streamDescriptor(), width: 4, height: 4, stride: 4, maxBytes: 24,
    format: 'i420', colorMatrix: 'bt709', colorRange: 'limited', sharedVideo: true, videoResource: 'reaweb://test/video/' } }); await flush();
  sockets[0].onopen(); const stream = await opening;
  assert.equal(stream.info.format, 'i420'); assert.equal(stream.info.colorMatrix, 'bt709');
  const packet = streamPacket(1, 1, new Uint8Array(4)), view = new DataView(packet.data);
  view.setUint8(5, 2); view.setBigUint64(32, 24n, true);
  sockets[0].onmessage(packet); await flush(); await flush();
  assert.equal(stream.latest().bytes.length, 24); assert.equal(stream.latest().bytes[23], 128);
  assert.equal(sockets[0].sent.length, 1);
  const closing = stream.close(); await flush(); t.reply(t.messages.length - 1, { result: true }); await closing;
});

test('Native video copies remain immutable and release mappings on invalid frames or close', async () => {
  for (const format of ['rgba8', 'i420']) for (const invalid of [false, true]) {
    const bytes = format === 'i420' ? 6 : 16;
    const t = await connected('webkit'), sockets = streamSockets(t), slots = new Uint8Array(bytes * 2), releases = [];
    t.context.ArrayBuffer = ArrayBuffer;
    let available = false;
    t.window.__reawebVideoCopy = (token, slot) => {
      assert.equal(token, 'ticket');
      if (slot === -2) { releases.push(token); return null; }
      if (slot === -1) return available ? bytes : null;
      return slots.slice(slot * bytes, slot * bytes + (invalid ? bytes - 1 : bytes)).buffer;
    };
    const pending = t.window.reaper.stream.open('reaweb.video.project.test'); await flush();
    t.reply(t.messages.length - 1, { result: { ...streamDescriptor(), format, maxBytes: bytes, stride: format === 'i420' ? 2 : 8, sharedVideo: true, videoNativeCopy: true } });
    await flush(); assert.equal(sockets.length, 0);
    available = true;
    const timer = [...t.timers.keys()].at(-1), wake = t.timers.get(timer);
    t.timers.delete(timer); wake(); await flush();
    const socket = sockets[0]; socket.onopen(); const stream = await pending, frames = [];
    stream.on('data', frame => frames.push(frame));
    assert.equal(stream.info.videoNativeCopy, undefined);
    for (let i = 0; i < 5; ++i) {
      slots.fill(i);
      const packet = streamPacket(i, 1, new Uint8Array(4)), view = new DataView(packet.data);
      view.setUint8(5, 2); view.setBigUint64(32, BigInt(bytes), true); view.setUint32(40, i % 2, true);
      socket.onmessage(packet);
    }
    if (invalid) {
      assert.equal(stream.error.code, 'UNSUPPORTED_FORMAT'); assert.equal(socket.sent.length, 0);
      assert.equal(frames.length, 0); assert.ok(releases.length);
    } else {
      assert.equal(socket.sent.length, 5);
      frames.forEach((frame, i) => assert.deepEqual([...frame.bytes], Array(bytes).fill(i)));
      const closing = stream.close(); await flush(); t.reply(t.messages.length - 1, { result: true }); await closing;
      assert.ok(releases.length); assert.equal(stream.closed, true);
    }
  }
});

test('Unavailable native video mapping fails without masking the transport error', async () => {
  const t = await connected('webkit'); t.window.__reawebVideoCopy = {};
  const pending = t.window.reaper.stream.open('reaweb.video.project.test'); await flush();
  t.reply(t.messages.length - 1, { result: { ...streamDescriptor(), sharedVideo: true, videoNativeCopy: true } });
  await flush(); assert.equal(t.messages.at(-1).method, 'ReaWeb_StreamDetach');
  t.reply(t.messages.length - 1, { result: true });
  await assert.rejects(pending, { code: 'TRANSPORT_ERROR' });
});

test('Audio consumers bound FIFO, drop incoming on overrun and return null on underrun', async () => {
  const t = await connected(), sockets = streamSockets(t);
  const pending = t.window.reaper.stream.open('test.audio'); await flush();
  t.reply(t.messages.length - 1, { result: streamDescriptor('audio', 'test.audio') }); await flush(); sockets[0].onopen();
  const stream = await pending;
  for (let i = 1; i <= 5; ++i) sockets[0].onmessage(streamPacket(i, 2));
  assert.equal(stream.dropped, 2); assert.equal(stream.latest().sequence, 5n);
  assert.ok(stream.read().data instanceof Float32Array);
  assert.equal(stream.read().sequence, 2n); assert.equal(stream.read().sequence, 3n); assert.equal(stream.read(), null);
  sockets[0].onmessage(streamPacket(6, 2)); assert.equal(stream.read().sequence, 6n);
  sockets[0].onclose({ reason: 'EXTENSION_UNLOADED' });
  assert.equal(stream.closed, true); assert.equal(stream.error.code, 'EXTENSION_UNLOADED'); assert.equal(stream.read(), null);
});

test('Video resource reads preserve order, ACK after copying and abort on detach', async () => {
  const t = await connected('webkit'), sockets = streamSockets(t), reads = [];
  t.context.AbortController = AbortController;
  t.context.fetch = (url, options) => new Promise(resolve => reads.push({ url, options, resolve }));
  const pending = t.window.reaper.stream.open('reaweb.video.project.test'); await flush();
  t.reply(t.messages.length - 1, { result: { ...streamDescriptor(), sharedVideo: true, videoResource: 'reaweb://test/__reaweb_video__/ticket/' } });
  await flush(); const socket = sockets[0]; socket.onopen(); const stream = await pending, frames = [];
  stream.on('data', f => frames.push(f));
  const packet = i => {
    const message = streamPacket(i, 1, new Uint8Array(4)), view = new DataView(message.data);
    view.setUint8(5, 2); view.setBigUint64(32, 16n, true); view.setUint32(40, i % 2, true);
    return message;
  };
  socket.onmessage(packet(0)); socket.onmessage(packet(1)); await flush();
  assert.equal(reads.length, 1, 'Resource requests are ordered with their stream metadata');
  assert.equal(socket.sent.length, 0, 'Native slots cannot be reused during a resource read');
  reads[0].resolve({ ok: true, arrayBuffer: async () => new Uint8Array(16).fill(7).buffer }); await flush();
  assert.equal(socket.sent.length, 1); assert.equal(reads.length, 2);
  reads[1].resolve({ ok: true, arrayBuffer: async () => new Uint8Array(16).fill(8).buffer }); await flush();
  assert.equal(socket.sent.length, 2); assert.deepEqual(frames.map(f => [...f.bytes]), [Array(16).fill(7), Array(16).fill(8)]);
  assert.equal(stream.info.videoResource, undefined);
  socket.onmessage(packet(2)); await flush();
  const closing = stream.close(); await flush();
  assert.equal(reads[2].options.signal.aborted, true);
  t.reply(t.messages.length - 1, { result: true }); await closing;
  reads[2].resolve({ ok: true, arrayBuffer: async () => new ArrayBuffer(16) }); await flush();
  assert.equal(frames.length, 2); assert.equal(socket.sent.length, 2, 'Closing cannot deliver a late resource or ACK its slot');
});

test('Malformed video resources close without ACKing incomplete pixels', async () => {
  const t = await connected('webkit'), sockets = streamSockets(t);
  t.context.AbortController = AbortController;
  t.context.fetch = async () => ({ ok: true, arrayBuffer: async () => new ArrayBuffer(15) });
  const pending = t.window.reaper.stream.open('reaweb.video.project.test'); await flush();
  t.reply(t.messages.length - 1, { result: { ...streamDescriptor(), sharedVideo: true, videoResource: 'reaweb://test/__reaweb_video__/ticket/' } });
  await flush(); sockets[0].onopen(); const stream = await pending;
  const message = streamPacket(0, 1, new Uint8Array(4)), view = new DataView(message.data);
  view.setUint8(5, 2); view.setBigUint64(32, 16n, true);
  sockets[0].onmessage(message); await flush();
  assert.equal(stream.error.code, 'UNSUPPORTED_FORMAT'); assert.equal(sockets[0].sent.length, 0);
});
test('Stream format mismatch and connection failure expose typed errors', async () => {
  const t = await connected(), sockets = streamSockets(t);
  const pending = t.window.reaper.stream.open('test.frames'); await flush();
  t.reply(t.messages.length - 1, { result: streamDescriptor() }); await flush(); sockets[0].onopen();
  const stream = await pending, errors = [];
  stream.on('error', error => errors.push(error.code));
  sockets[0].onmessage(streamPacket(1, 2));
  assert.deepEqual(errors, ['UNSUPPORTED_FORMAT']); assert.equal(stream.error.code, 'UNSUPPORTED_FORMAT');
  assert.equal(stream.closed, true);
  const unavailable = t.window.reaper.stream.open('missing'); await flush();
  t.reply(t.messages.length - 1, { error: { code: 'STREAM_NOT_FOUND', message: 'missing' } });
  await assert.rejects(unavailable, { code: 'STREAM_NOT_FOUND' });
});


test('Built-in Meter decodes exact uint64 counts without changing Float32 measurements or third-party streams', async () => {
  const t = await connected(), decode = t.window.reaper.audio.decodeMeter;
  const counts = [0n, (1n << 24n) + 1n, (1n << 53n) + 1n, (1n << 64n) - 1n];
  const channels = counts.length, data = new Float32Array(13 * channels + 14), base = 7 * channels;
  data[0] = .5; data[base + 3] = -12.5;
  for (let group = 0; group < 2; ++group) counts.forEach((count, ch) => {
    const at = base + 14 + group * 3 * channels + ch;
    data[at] = Number(count & 0xffffffn);
    data[at + channels] = Number((count >> 24n) & 0xffffffn);
    data[at + 2 * channels] = Number(count >> 48n);
  });
  const original = data.slice(), values = decode(data, channels);
  assert.deepEqual(Array.from(values.sampleClipCount), counts);
  assert.deepEqual(Array.from(values.truePeakClipCount), counts);
  assert.equal(values.samplePeak[0], .5); assert.equal(values.lufsMomentary, -12.5);
  assert.deepEqual(data, original);
  assert.throws(() => decode(new Float32Array(5), channels), /built-in meter/);
  data[base + 14] = NaN; assert.throws(() => decode(data, channels), /counter/);
});

for (const engine of ['windows', 'webkit']) test(`Diagnostics probes bypass API queues and preserve privacy (${engine})`, async () => {
  const t = await connected(engine);
  const probe = n => t.window.__reawebReceive({ document: t.messages[0].document, diagnosticProbe: n });
  const mode = enabled => t.window.__reawebReceive({ document: t.messages[0].document, diagnosticEnabled: enabled });
  probe(0); probe(1);
  assert.equal(t.reports.length, 0);
  mode(true);
  const request = t.window.reaper.CountTracks();
  await flush();
  const index = t.messages.length - 1;
  t.reply(index, { started: true });
  probe(1);
  assert.equal(t.reports[0].pagePending, 1);
  assert.equal(t.reports[0].oldestStarted, 1);
  assert.equal(t.reports[0].method, 'CountTracks');
  assert.equal(t.timers.size, 0);
  t.window.__reawebReceive({ document: 'stale', diagnosticProbe: 2 });
  assert.equal(t.reports.length, 1);
  t.reply(index, { result: 0 }); await request;
  t.listeners.error({ message: 'secret error', filename: 'secret-url', lineno: 42, colno: 9 });
  await flush();
  t.reply(t.messages.length - 1, { result: true }); await flush();
  probe(3);
  assert.equal(t.reports[1].scriptErrors, 1);
  assert.equal(t.reports[1].scriptLine, 42);
  assert.equal(t.reports[1].scriptColumn, 9);
  assert.equal(JSON.stringify(t.reports).includes('secret'), false);
  const timeout = assert.rejects(t.window.reaper.CountTracks(), { code: 'TIMEOUT' });
  await flush();
  for (const timer of [...t.timers.values()]) timer();
  await timeout; t.timers.clear();
  probe(4);
  assert.equal(t.reports[2].timeouts, 1);
  assert.equal(t.reports[2].timeoutMethod, 'CountTracks');
  assert.equal(t.reports[2].pagePending, 0);
  const outstanding = t.window.reaper.CountTracks(); await flush();
  const outstandingIndex = t.messages.length - 1;
  t.reply(outstandingIndex, { started: true });
  mode(false); probe(5);
  assert.equal(t.reports.length, 3);
  t.listeners.error({ message: 'disabled error', lineno: 99 }); await flush();
  t.reply(t.messages.length - 1, { result: true }); await flush();
  t.window.__reawebReceive({ document: 'stale', diagnosticEnabled: true }); probe(6);
  assert.equal(t.reports.length, 3);
  mode(true); probe(7);
  assert.equal(t.reports[3].scriptErrors, 0);
  assert.equal(t.reports[3].timeouts, 0);
  assert.equal(t.reports[3].pagePending, 1);
  assert.equal(t.reports[3].oldestStarted, 1);
  assert.equal(t.reports[3].method, '');
  t.reply(outstandingIndex, { result: 3 }); assert.equal(await outstanding, 3);
  t.listeners.pagehide(); probe(8);
  assert.equal(t.reports.length, 4);
});

test('Realtime decodes transport snapshots without altering stream timing and releases once', async () => {
  const t = await connected(), sockets = streamSockets(t), received = [];
  const pending = t.window.reaper.realtime.subscribe('transport', {updateRate:120, onData: value=>received.push(value)});
  await flush();
  assert.equal(t.messages.at(-1).method, 'ReaWeb_RealtimeSubscribe');
  assert.deepEqual(t.messages.at(-1).args, ['transport',{updateRate:120}]);
  t.reply(t.messages.length-1,{result:{id:7,stream:'reaweb.realtime.transport.7',schema:'transport.v1',updateRate:120}});
  await flush();
  assert.equal(t.messages.at(-1).method,'ReaWeb_StreamOpen');
  t.reply(t.messages.length-1,{result:{...streamDescriptor('binary'),format:'bytes',maxBytes:64}});
  await flush(); sockets[0].onopen();
  const subscription = await pending;
  const bytes=new Uint8Array(64), view=new DataView(bytes.buffer);
  view.setUint32(0,0x31305452,true);view.setFloat64(8,123,true);view.setFloat64(16,4.5,true);
  view.setUint32(24,5,true);view.setBigUint64(32,3n,true);view.setBigUint64(48,200n,true);
  sockets[0].onmessage(streamPacket(1,6,bytes));
  assert.equal(received.length,1);assert.equal(received[0].position,4.5);assert.equal(received[0].playState,5);
  assert.equal(received[0].projectEpoch,3n);assert.equal(received[0].sourceSequence,200n);
  assert.equal(received[0].sampledAt,123);assert.ok(Object.isFrozen(received[0]));
  assert.deepEqual(Object.keys(received[0]).sort(), ['source','position','playState','sampledAt','sampleDurationMs','sequence','sourceSequence','projectEpoch','projectChanged','producerDropped'].sort());
  view.setUint32(28,2,true);view.setFloat64(56,4.6,true);
  sockets[0].onmessage(streamPacket(2,6,bytes));
  assert.equal(received[1].position,4.5);assert.deepEqual(Object.keys(received[1]),Object.keys(received[0]));
  const before=t.messages.length, close=subscription.close();await flush();
  const closing=t.messages.slice(before);
  assert.deepEqual(closing.map(x=>x.method).sort(),['ReaWeb_RealtimeUnsubscribe','ReaWeb_StreamDetach']);
  for(let i=before;i<t.messages.length;i++)t.reply(i,{result:true});
  await close;await subscription.close();
  assert.equal(subscription.closed,true);assert.equal(subscription.latest(),null);
  assert.equal(t.messages.length,before+2);
});

test('Realtime decodes musical transport snapshots without altering stream timing and releases once', async () => {
  const t = await connected(), sockets = streamSockets(t), received = [];
  const pending = t.window.reaper.realtime.subscribe('transport', {updateRate:120, onData: value=>received.push(value)});
  await flush();
  assert.equal(t.messages.at(-1).method, 'ReaWeb_RealtimeSubscribe');
  assert.deepEqual(t.messages.at(-1).args, ['transport',{updateRate:120}]);
  t.reply(t.messages.length-1,{result:{id:7,stream:'reaweb.realtime.transport.7',schema:'transport.v2',updateRate:120}});
  await flush();
  assert.equal(t.messages.at(-1).method,'ReaWeb_StreamOpen');
  t.reply(t.messages.length-1,{result:{...streamDescriptor('binary'),format:'bytes',maxBytes:96}});
  await flush(); sockets[0].onopen();
  const subscription = await pending;
  const bytes=new Uint8Array(96), view=new DataView(bytes.buffer);
  view.setUint32(0,0x32305452,true);view.setFloat64(8,123,true);view.setFloat64(16,4.5,true);
  view.setUint32(24,5,true);view.setBigUint64(32,3n,true);view.setBigUint64(48,200n,true);
  view.setFloat64(64,1.25,true);view.setFloat64(72,137.5,true);view.setFloat64(80,12.625,true);
  view.setUint32(88,7,true);view.setUint32(92,8,true);
  sockets[0].onmessage(streamPacket(1,6,bytes));
  assert.equal(received[0].playRate,1.25);assert.equal(received[0].tempo,137.5);assert.equal(received[0].beatPosition,12.625);
  assert.equal(received[0].timeSignature.numerator,7);assert.equal(received[0].timeSignature.denominator,8);assert.ok(Object.isFrozen(received[0].timeSignature));
  assert.equal(received.length,1);assert.equal(received[0].position,4.5);assert.equal(received[0].playState,5);
  assert.equal(received[0].projectEpoch,3n);assert.equal(received[0].sourceSequence,200n);
  assert.equal(received[0].sampledAt,123);assert.ok(Object.isFrozen(received[0]));
  assert.deepEqual(Object.keys(received[0]).sort(), ['source','position','playState','playRate','tempo','timeSignature','beatPosition','sampledAt','sampleDurationMs','sequence','sourceSequence','projectEpoch','projectChanged','producerDropped'].sort());
  view.setUint32(28,2,true);view.setFloat64(56,4.6,true);
  sockets[0].onmessage(streamPacket(2,6,bytes));
  assert.equal(received[1].position,4.5);assert.deepEqual(Object.keys(received[1]),Object.keys(received[0]));
  for (const [offset, value] of [[64,0],[64,NaN],[72,-1],[72,Infinity],[80,NaN]]) {
    const saved=view.getFloat64(offset,true);view.setFloat64(offset,value,true);
    sockets[0].onmessage(streamPacket(3,6,bytes));
    assert.equal(received.length,2);view.setFloat64(offset,saved,true);
  }

  const before=t.messages.length, close=subscription.close();await flush();
  const closing=t.messages.slice(before);
  assert.deepEqual(closing.map(x=>x.method).sort(),['ReaWeb_RealtimeUnsubscribe','ReaWeb_StreamDetach']);
  for(let i=before;i<t.messages.length;i++)t.reply(i,{result:true});
  await close;await subscription.close();
  assert.equal(subscription.closed,true);assert.equal(subscription.latest(),null);
  assert.equal(t.messages.length,before+2);
});

test('Realtime rejects invalid options before allocation and cleans up failed attachments', async()=>{
  const t=await connected();
  for(const options of [{updateRate:0},{updateRate:121},{updateRate:NaN},{onData:3},{unknown:true},null])
    await assert.rejects(t.window.reaper.realtime.subscribe('transport',options),{code:'INVALID_ARGUMENT'});
  const pending=assert.rejects(t.window.reaper.realtime.subscribe('transport'),{code:'STREAM_NOT_FOUND'});
  await flush();t.reply(t.messages.length-1,{result:{id:9,stream:'missing',schema:'transport.v1'}});
  await flush();t.reply(t.messages.length-1,{error:{code:'STREAM_NOT_FOUND',message:'missing'}});
  await flush();assert.equal(t.messages.at(-1).method,'ReaWeb_RealtimeUnsubscribe');
  t.reply(t.messages.length-1,{result:true});await pending;
});

test('Video source metadata preserves final dimensions and unavailable states', async () => {
  const t = await connected();
  for (const value of [{available:true,width:2560,height:1440,originalSupported:true,error:null},
      {available:false,width:0,height:0,originalSupported:false,error:'VIDEO_FRAME_PENDING'}]) {
    const result = await answer(t, 'ReaWeb_VideoSourceInfo', [], value, t.window.reaper.video.getSourceInfo());
    assert.equal(result.width, value.width); assert.equal(result.available, value.available); assert.equal(result.error, value.error);
  }
});

async function surfaceFixture() {
  const events = {}, document = { hidden: false, addEventListener: (name, callback) => { events[name] = callback; } };
  const t = setup('webkit', document);
  t.reply(0, { result: { protocol: 1, projectEpoch: 1, methods, batchMethods, events: hostEvents } });
  await t.window.reaper.lifecycle.ready;
  return { ...t, document, documentEvents: events };
}
const surfaceDescriptor = (renderer = 'native-metal') => ({ ...streamDescriptor(), renderer, width: 3840, height: 2160 });
async function openSurfaceFixture(t, renderer = 'native-metal') {
  return answer(t, 'ReaWeb_VideoSurfaceOpen', [{ resolution: 'source' }], surfaceDescriptor(renderer), t.window.reaper.video.openSurface({ resolution: 'source' }));
}
for (const renderer of ['native-metal', 'native-d3d11']) test(`${renderer} surfaces preserve their explicit non-pixel contract and close once`, async () => {
  const t = await surfaceFixture(), surface = await openSurfaceFixture(t, renderer);
  assert.ok(Object.isFrozen(surface)); assert.ok(Object.isFrozen(surface.info));
  assert.equal(surface.info.renderer, renderer); assert.equal(surface.info.token, undefined); assert.equal(surface.info.url, undefined);
  assert.equal(surface.readablePixels, false); assert.equal(surface.read, undefined); assert.equal(surface.latest, undefined);
  assert.throws(() => surface.on('data', () => {}), { code: 'INVALID_ARGUMENT' });
  const layout = { rect: { x: -100, y: 25, width: 1920, height: 1080 }, clip: { x: 0, y: 25, width: 800, height: 600 }, preserveAspect: true };
  await answer(t, 'ReaWeb_VideoSurfaceLayout', ['ticket', layout], true, surface.setLayout(layout));
  const diagnostics = { closed: false, hasFrame: true, presentedFrames: 60, submittedFrames: 61, renderer };
  assert.deepEqual(JSON.parse(JSON.stringify(await answer(t, 'ReaWeb_VideoSurfaceDiagnostics', ['ticket'], diagnostics, surface.getDiagnostics()))), diagnostics);
  if (renderer === 'native-d3d11') {
    const unavailable = { closed: false, renderer, presentationCounter: 'dxgi-observed', presentationAvailable: false, presentedFrames: null, presentationFPS: null };
    assert.deepEqual(JSON.parse(JSON.stringify(await answer(t, 'ReaWeb_VideoSurfaceDiagnostics', ['ticket'], unavailable, surface.getDiagnostics()))), unavailable);
    assert.equal(surface.closed, false);
  }
  let closed = 0; surface.on('close', () => ++closed);
  const closing = surface.close(); assert.equal(surface.close(), closing);
  await answer(t, 'ReaWeb_VideoSurfaceClose', ['ticket'], true, closing);
  const count = t.messages.length; await surface.close(); assert.equal(t.messages.length, count);
  assert.equal(closed, 1); assert.equal(surface.closed, true);
  await assert.rejects(surface.setLayout(layout), { code: 'STREAM_CLOSED' });
});
test('Native surface diagnostics close remote-ended subscriptions without a fake data event', async () => {
  const t = await surfaceFixture(), surface = await openSurfaceFixture(t), events = [];
  surface.on('error', error => events.push(error.code)); surface.on('close', error => events.push(error.code));
  const pending = surface.getDiagnostics(); await flush();
  t.reply(t.messages.length - 1, { result: { closed: true, hasFrame: false, errorCode: 'VIDEO_PROJECT_CHANGED', error: 'Project changed' } });
  await flush(); assert.equal(t.messages.at(-1).method, 'ReaWeb_VideoSurfaceClose');
  t.reply(t.messages.length - 1, { result: true });
  assert.equal((await pending).closed, true); assert.equal(surface.closed, true);
  assert.deepEqual(events, ['VIDEO_PROJECT_CHANGED', 'VIDEO_PROJECT_CHANGED']);
});
test('Native surface failures preserve stable renderer error codes and messages', async () => {
  for (const result of [
    { closed: true, errorCode: 'VIDEO_RENDERER_UNAVAILABLE', error: 'D3D initialization failed' },
    { closed: true, error: 'Metal command buffer failed' },
    { closed: true }
  ]) {
    const t = await surfaceFixture(), surface = await openSurfaceFixture(t), errors = [], closes = [];
    surface.on('error', error => errors.push(error)); surface.on('close', error => closes.push(error));
    const pending = surface.getDiagnostics(); await flush(); t.reply(t.messages.length - 1, { result });
    await flush(); assert.equal(t.messages.at(-1).method, 'ReaWeb_VideoSurfaceClose');
    t.reply(t.messages.length - 1, { result: true }); await pending;
    assert.equal(closes.length, 1); assert.equal(errors.length, result.error ? 1 : 0);
    assert.equal(closes[0].code, result.error ? 'VIDEO_RENDERER_UNAVAILABLE' : 'STREAM_CLOSED');
    assert.equal(closes[0].message, result.error || 'Video surface closed');
  }
});
test('Native surfaces close on hidden pages and release an attachment arriving after hiding', async () => {
  for (const duringOpen of [false, true]) {
    const t = await surfaceFixture();
    const opening = t.window.reaper.video.openSurface({ resolution: 'source' }); await flush();
    const index = t.messages.length - 1;
    let surface, rejected;
    if (!duringOpen) { t.reply(index, { result: surfaceDescriptor() }); surface = await opening; }
    else rejected = assert.rejects(opening, { code: 'STREAM_CLOSED' });
    t.document.hidden = true; t.documentEvents.visibilitychange();
    if (duringOpen) t.reply(index, { result: surfaceDescriptor() });
    await flush(); assert.equal(t.messages.at(-1).method, 'ReaWeb_VideoSurfaceClose');
    t.reply(t.messages.length - 1, { result: true });
    if (duringOpen) await rejected; else { await surface.close(); assert.equal(surface.closed, true); }
    const count = t.messages.length;
    await assert.rejects(t.window.reaper.video.openSurface(), { code: 'VIDEO_PAGE_HIDDEN' });
    assert.equal(t.messages.length, count);
  }
});
test('Surface failures retain profile conflicts and page unload clears ownership without new RPCs', async () => {
  const t = await surfaceFixture();
  const rejected = assert.rejects(t.window.reaper.video.openSurface(), { code: 'VIDEO_PROFILE_CONFLICT' });
  await flush(); t.reply(t.messages.length - 1, { error: { code: 'VIDEO_PROFILE_CONFLICT', message: 'Active profile' } }); await rejected;
  const surface = await openSurfaceFixture(t), count = t.messages.length;
  t.listeners.pagehide(); await flush();
  assert.equal(surface.closed, true); assert.equal(t.messages.length, count);
});

test('file writers snapshot chunks, advance acknowledged offsets and reject concurrent writes', async () => {
  const t=await connected(), io=t.window.reaper.fs;
  const writer=await answer(t,'ReaWeb_OpenWrite',['large.bin',{}],{id:'writer',maxChunkBytes:1048576},io.openWrite('large.bin'));
  assert.ok(Object.isFrozen(writer));
  const bytes=new Uint8Array([1,2,3]), pending=writer.write(bytes);bytes.fill(9);
  await assert.rejects(writer.write(new Uint8Array([4])),{code:'WRITE_BUSY'});
  await answer(t,'ReaWeb_WriteChunk',['writer',{__reawebBytes:'AQID'},0],{bytes:3,totalBytes:3},pending);
  await answer(t,'ReaWeb_WriteChunk',['writer',{__reawebBytes:'BA=='},3],{bytes:1,totalBytes:4},writer.write(new Uint8Array([4])));
  await assert.rejects(writer.write(new Uint8Array(1048577)),{code:'BUFFER_LIMIT'});
  const result=await answer(t,'ReaWeb_CommitWrite',['writer'],{state:'committed',path:'large.bin',bytes:4},writer.commit());
  assert.deepEqual({...result},{path:'large.bin',bytes:4});
  await answer(t,'ReaWeb_WriteStatus',['writer'],{state:'committed',path:'large.bin',bytes:4,cleanupPending:false},writer.status());
});
