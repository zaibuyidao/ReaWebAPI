// Loaded by the native WebKit acceptance harnesses, not by the Node test runner.
(async () => {
  const result = { origin: location.origin, userAgent: navigator.userAgent,
    secure: isSecureContext, isolated: crossOriginIsolated, sab: typeof SharedArrayBuffer === 'function',
    wasmSharedMemory: false, worker: null, blobWorker: null, errors: {} };
  try { result.wasmSharedMemory = new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true }).buffer instanceof SharedArrayBuffer; }
  catch (error) { result.errors.memory = String(error); }
  const run = url => new Promise(resolve => {
    let worker;
    const finish = value => { clearTimeout(timer); worker?.terminate(); resolve(value); };
    const timer = setTimeout(() => finish({ timeout: true }), 3000);
    try {
      worker = new Worker(url);
      worker.onerror = event => finish({ error: event.message || 'Worker error' });
      worker.onmessage = event => finish(event.data);
      const message = { token: 'webkit-acceptance' };
      if (result.sab) message.sab = new SharedArrayBuffer(4);
      if (result.wasmSharedMemory) {
        message.memory = new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true });
        Atomics.store(new Int32Array(message.memory.buffer), 0, 17);
      }
      worker.postMessage(message);
    } catch (error) { finish({ error: String(error) }); }
  });
  const url = new URL('/__reaweb_runtime__/probe.js', location.href);
  result.worker = await run(url.href);
  let blob;
  try {
    const response = await fetch(url);
    result.responseHeaders = Object.fromEntries(response.headers);
    if (!response.ok) throw new Error('Probe HTTP status ' + response.status);
    blob = URL.createObjectURL(new Blob([await response.text()], { type: 'text/javascript' }));
    result.blobWorker = await run(blob);
  } catch (error) { result.errors.blob = String(error); }
  finally { if (blob) URL.revokeObjectURL(blob); }
  const canvas = document.createElement('canvas'), gl = canvas.getContext('webgl2');
  result.webgl2 = !!gl;
  gl?.getExtension('WEBGL_lose_context')?.loseContext();
  window.webkit.messageHandlers.reaweb.postMessage('PROBE:' + JSON.stringify(result));
})();
