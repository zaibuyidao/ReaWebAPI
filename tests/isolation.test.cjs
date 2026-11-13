const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { webcrypto } = require('node:crypto');
const methods = Object.keys(JSON.parse(fs.readFileSync('api/bindings.json')).functions);
const source = fs.readFileSync('runtime/reaper-api.generated.js', 'utf8') + fs.readFileSync('runtime/reaper.js', 'utf8');
function setup({ required = false, isolated = true, workerMode = 'success', loading = false } = {}) {
  const counts = { workers: 0, terminated: 0, revoked: 0, stopped: 0, fetches: 0 };
  let browser = {}, failure;
  const window = { isSecureContext: true, crossOriginIsolated: isolated, SharedArrayBuffer,
    location: { protocol: 'reaweb:', origin: 'reaweb://test' },
    document: { readyState: loading ? 'loading' : 'complete', createElement: () => ({ getContext: () => null }) },
    addEventListener() {}, removeEventListener() {}, stop() { ++counts.stopped; } };
  window.top = window;
  window.Worker = class {
    constructor() { ++counts.workers; if (workerMode === 'blocked') throw new Error('CSP'); }
    terminate() { ++counts.terminated; }
    postMessage({ token, memory }) {
      if (workerMode === 'timeout') return;
      if (memory) Atomics.add(new Int32Array(memory.buffer), 0, 1);
      queueMicrotask(() => this.onmessage({ data: { token, crossOriginIsolated: isolated,
        sharedArrayBuffer: true, wasmSharedMemory: true, sharedRoundTrip: !!memory } }));
    }
  };
  window.chrome = { webview: { addEventListener() {}, postMessage(text) {
    if (text.startsWith('__reawebBrowser:')) {
      const report = JSON.parse(text.slice(16)); browser = report.browser; failure = report.error; return;
    }
    const request = JSON.parse(text);
    queueMicrotask(() => window.__reawebReceive({ id: request.id, document: request.document, result:
      request.method === '__reawebHello' ? { protocol: 1, projectEpoch: 1, methods, events: [] } :
      { webRuntime: { browser } } }));
  } } };
  const context = { window, crypto: webcrypto, console, TextEncoder, TextDecoder, Uint8Array, Uint32Array, Int32Array,
    Float64Array, ArrayBuffer, SharedArrayBuffer, Atomics, WebAssembly, AbortController, Blob,
    URL: { createObjectURL: () => 'blob:test', revokeObjectURL: () => ++counts.revoked },
    fetch: async () => { ++counts.fetches; return { ok: true, text: async () => '' }; },
    setTimeout: (fn, ms) => setTimeout(fn, ms === 3000 ? 15 : ms), clearTimeout,
    setInterval, clearInterval, queueMicrotask, reawebIsolationRequired: required };
  vm.runInNewContext(source, context);
  return { window, counts, get failure() { return failure; } };
}
test('browser probes run once, use actual messages and clean up both Worker forms', async () => {
  const env = setup();
  await env.window.reaper.lifecycle.ready;
  const [a, b] = await Promise.all([env.window.reaper.system.getCapabilities(), env.window.reaper.debug.getDiagnostics()]);
  assert.equal(a.webRuntime.browser.worker, true);
  assert.equal(a.webRuntime.browser.blobWorker, true);
  assert.equal(a.webRuntime.browser.wasmSharedMemory, true);
  assert.equal(b.webRuntime.browser.webgl2, false);
  assert.deepEqual(env.counts, { workers: 2, terminated: 2, revoked: 1, stopped: 0, fetches: 1 });
});
test('CSP rejection is false while probe timeout remains unknown', async () => {
  for (const mode of ['blocked', 'timeout']) {
    const env = setup({ workerMode: mode });
    const { webRuntime } = await env.window.reaper.system.getCapabilities();
    assert.equal(webRuntime.browser.worker, mode === 'blocked' ? false : null);
    assert.equal(webRuntime.browser.blobWorker, mode === 'blocked' ? false : null);
    assert.equal(webRuntime.browser.errors.worker, mode === 'blocked' ? 'WORKER_CREATE_OR_SEND_FAILED' : 'PROBE_TIMEOUT');
    assert.equal(env.counts.revoked, 1);
    if (mode === 'timeout') assert.equal(env.counts.terminated, 2);
  }
});
test('required failure reports to the host before exposing the runtime', () => {
  const env = setup({ required: true, isolated: false });
  assert.equal(env.window.reaper, undefined);
  assert.equal(env.failure, 'CROSS_ORIGIN_ISOLATION_FAILED');
  assert.equal(env.counts.stopped, 1);
  assert.equal(env.counts.workers, 0);
  const unsupported = setup({ required: true, isolated: null });
  assert.equal(unsupported.failure, 'CROSS_ORIGIN_ISOLATION_UNSUPPORTED');
});
test('a stalled document cannot leave capability queries waiting indefinitely', async () => {
  const env = setup({ loading: true });
  const { webRuntime } = await env.window.reaper.system.getCapabilities();
  assert.equal(webRuntime.browser.worker, null);
  assert.equal(webRuntime.browser.errors.worker, 'DOCUMENT_NOT_READY');
  assert.equal(env.counts.workers, 0);
});
