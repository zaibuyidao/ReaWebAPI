'use strict';
onmessage = async ({ data }) => {
  if (data.crash) { setTimeout(() => { throw new Error('Intentional Worker failure'); }, 0); return; }
  try {
    importScripts(data.base + 'dependency.js');
    const response = await fetch(data.base + 'atomic.wasm');
    if (!response.ok) throw new Error(`WASM resource failed: HTTP ${response.status}`);
    const { instance } = await WebAssembly.instantiateStreaming(response, { env: { memory: data.memory } });
    const view = new Int32Array(data.memory.buffer), separate = new Int32Array(data.buffer);
    postMessage({ waiting: true, isolated: self.crossOriginIsolated, sab: data.buffer instanceof SharedArrayBuffer });
    const waited = Atomics.wait(view, 1, 0, 5000);
    for (let i = 0; i < self.iterations; ++i) instance.exports.add(data.increment);
    Atomics.add(separate, 0, data.increment);
    postMessage({ done: true, waited, increment: data.increment });
    close();
  } catch (error) { postMessage({ error: String(error) }); close(); }
};
