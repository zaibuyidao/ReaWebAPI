const { test } = require('node:test');
const assert = require('node:assert/strict');
const sdk = import('../runtime/browser-files.mjs');

test('binary export preserves bytes, strips paths and handles cancellation before reading', async () => {
  const { saveBlob, MAX_DOWNLOAD_BYTES } = await sdk;
  const calls = [];
  const host = {
    dialog: { saveFile: async options => { calls.push(options); return '/chosen/entry.eqf'; } },
    fs: { writeBinary: async (...args) => calls.push(args) }
  };
  assert.equal(await saveBlob(new Blob([new Uint8Array([0, 255, 13, 10])]), '../entry.eqf', host), '/chosen/entry.eqf');
  assert.equal(calls[0].initialPath, 'entry.eqf');
  assert.deepEqual([...calls[1][1]], [0, 255, 13, 10]);
  assert.deepEqual(calls[1][2], { overwrite:true });
  host.dialog.saveFile = async () => null;
  const cancelled = new Blob(['x']);
  cancelled.arrayBuffer = () => { throw Error('must not read'); };
  assert.equal(await saveBlob(cancelled, 'x', host), null);
  await assert.rejects(saveBlob(new Blob([new Uint8Array(MAX_DOWNLOAD_BYTES + 1)]), 'x', host), { code:'BUFFER_LIMIT' });
});

test('download handler filters URLs, serializes dialogs, reports failures and detaches', async () => {
  const { installDownloadHandler } = await sdk;
  let listener, writes = [], errors = [], finish;
  const doc = { addEventListener: (_, fn) => listener = fn, removeEventListener: (_, fn) => { assert.equal(fn, listener); listener = null; } };
  const host = {
    dialog:{ saveFile: () => new Promise(resolve => finish = resolve) },
    fs:{ writeBinary: async (path, bytes) => writes.push([path, [...bytes]]) }
  };
  const dispose = installDownloadHandler({ document:doc, host, onError:e => errors.push(e) });
  function click(href, prevented = false) {
    const event = { defaultPrevented:prevented, target:{closest:() => ({href,download:'x.eqf'})}, preventDefault() { this.defaultPrevented = true; } };
    listener(event); return event;
  }
  assert.equal(click('https://example.com').defaultPrevented, false);
  assert.equal(click('file:///private').defaultPrevented, false);
  assert.equal(click('data:application/octet-stream;base64,AP8=').defaultPrevented, true);
  while (!finish) await new Promise(setImmediate);
  click('data:text/plain,ignored');
  finish('/chosen');
  while (!writes.length) await new Promise(setImmediate);
  await new Promise(setImmediate);
  assert.deepEqual(writes, [['/chosen', [0, 255]]]);
  host.dialog.saveFile = async () => { throw Error('dialog unavailable'); };
  click('data:text/plain,hello');
  while (!errors.length) await new Promise(setImmediate);
  while (errors.length < 2) await new Promise(setImmediate);
  assert.equal(errors[0].code, 'WRITE_BUSY');
  assert.equal(errors[1].message, 'dialog unavailable');
  dispose(); assert.equal(listener, null);
});

function streamingHost() {
  const state = { bytes:0, writes:[], committed:false, aborted:false };
  const writer = {
    maxChunkBytes:1024*1024,
    async write(chunk) { state.writes.push(chunk.byteLength); state.bytes += chunk.byteLength; return {totalBytes:state.bytes}; },
    async commit() { state.committed = true; return {path:'/chosen',bytes:state.bytes}; },
    async abort() { state.aborted = true; return {state:state.committed ? 'committed':'aborted',bytes:state.bytes}; }
  };
  const host = {dialog:{saveFile:async()=>'/chosen'},fs:{openWrite:async(path,options)=>{state.options=options;return writer;}}};
  return {host,writer,state};
}
test('large Blob uses bounded slices and sequential acknowledgements', async () => {
  const {saveBlob,MAX_DOWNLOAD_BYTES} = await sdk;
  const {host,state} = streamingHost();
  const blob = new Blob([new Uint8Array(MAX_DOWNLOAD_BYTES+1)]);
  blob.arrayBuffer = () => {throw Error('whole Blob read');};
  const events=[];
  assert.equal(await saveBlob(blob,'large',host,{onProgress:p=>events.push(p)}),'/chosen');
  assert.equal(state.bytes,blob.size);
  assert.ok(state.writes.every(n=>n<=256*1024));
  assert.deepEqual(state.options,{overwrite:true,expectedBytes:blob.size});
  assert.equal(events.at(-1).state,'committed');
  assert.equal(state.aborted,false);
});
test('cancellation at write and commit boundary aborts without publishing', async () => {
  const {saveBlob} = await sdk;
  for (const boundary of ['writing','committing']) {
    const {host,state}=streamingHost(), controller=new AbortController();
    await assert.rejects(saveBlob(new Blob(['abc']),'x',host,{signal:controller.signal,onProgress:p=>{if(p.state===boundary)controller.abort();}}),{code:'WRITE_CANCELLED'});
    assert.equal(state.aborted,true); assert.equal(state.committed,false);
  }
});
test('lost commit reply is resolved from native terminal state', async () => {
  const {saveBlob}=await sdk;
  const {host,writer,state}=streamingHost();
  writer.commit=async()=>{state.committed=true;throw Error('reply lost');};
  assert.equal(await saveBlob(new Blob(['x']),'x',host),'/chosen');
  assert.equal(state.aborted,true);
});
test('new SDK falls back to the existing API on older hosts', async () => {
  const {saveBlob,MAX_DOWNLOAD_BYTES}=await sdk;
  const {host}=streamingHost(); let wrote=false;
  host.system={getCapabilities:async()=>({runtime:{}})};
  host.fs.openWrite=()=>{throw Error('unsupported call');};
  host.fs.writeBinary=async()=>{wrote=true;};
  await saveBlob(new Blob(['x']),'x',host);assert.equal(wrote,true);
  await assert.rejects(saveBlob(new Blob([new Uint8Array(MAX_DOWNLOAD_BYTES+1)]),'x',host),{code:'BUFFER_LIMIT'});
});
test('Blob download acquires the URL before immediate revocation', async () => {
  const {installDownloadHandler}=await sdk;
  const {host,state}=streamingHost(); let listener, finish;
  const done=new Promise(resolve=>finish=resolve), errors=[];
  const doc={addEventListener:(_,fn)=>listener=fn,removeEventListener:()=>{}};
  const dispose=installDownloadHandler({host,document:doc,onError:e=>{errors.push(e);finish();},onProgress:p=>{if(p.state==='committed')finish();}});
  const url=URL.createObjectURL(new Blob([new Uint8Array(17*1024*1024)]));
  listener({target:{closest:()=>({href:url,download:'large'})},preventDefault(){}});
  URL.revokeObjectURL(url);
  await done;dispose();assert.deepEqual(errors,[]);assert.equal(state.bytes,17*1024*1024);
});
