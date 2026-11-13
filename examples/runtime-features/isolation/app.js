const check = (condition, message) => { if (!condition) throw new Error(message); };
const base = new URL('.', location.href).href;
async function runWorkers(blob) {
  const memory = new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true });
  const buffer = new SharedArrayBuffer(4), view = new Int32Array(memory.buffer);
  const workers = [], results = [];
  let url = base + 'worker.js', objectUrl, timeout, wake;
  try {
    if (blob) url = objectUrl = URL.createObjectURL(new Blob([await (await fetch(url)).text()], { type: 'text/javascript' }));
    await new Promise((resolve, reject) => {
      let waiting = 0;
      timeout = setTimeout(() => reject(new Error('Worker test timed out')), 10000);
      for (let i = 1; i <= 4; ++i) {
        const worker = new Worker(url); workers.push(worker);
        worker.onerror = event => reject(new Error(event.message));
        worker.onmessageerror = () => reject(new Error('Shared memory clone failed'));
        worker.onmessage = ({ data }) => {
          if (data.error) return reject(new Error(data.error));
          if (data.waiting) {
            if (!data.isolated || !data.sab) return reject(new Error('Worker isolation or SAB failed'));
            if (++waiting === 4) wake = setTimeout(() => {
              Atomics.store(view, 1, 1);
              Atomics.notify(view, 1, 4);
            }, 100);
          }
          if (data.done) {
            results.push(data);
            if (results.length === 4) resolve();
          }
        };
        worker.postMessage({ base, memory, buffer, increment: i });
      }
    });
    check(Atomics.load(view, 0) === 100000, 'WASM atomic computation mismatch');
    check(Atomics.load(new Int32Array(buffer), 0) === 10, 'SAB atomic computation mismatch');
    check(results.every(result => result.waited === 'ok'), 'Atomics wait/notify failed');
    return { kind: blob ? 'blob' : 'url', workers: 4, wasmResult: view[0], sabResult: new Int32Array(buffer)[0], waits: results.map(item => item.waited) };
  } finally {
    clearTimeout(timeout); clearTimeout(wake);
    for (const worker of workers) worker.terminate();
    if (objectUrl) URL.revokeObjectURL(objectUrl);
  }
}
async function workerFailure() {
  const worker = new Worker(base + 'worker.js');
  let timeout;
  try {
    await new Promise((resolve, reject) => {
      timeout = setTimeout(() => reject(new Error('Worker error was not delivered')), 3000);
      worker.onerror = event => { event.preventDefault(); resolve(); };
      worker.postMessage({ crash: true });
    });
    return true;
  } finally { clearTimeout(timeout); worker.terminate(); }
}
window.isolationCheck = (async () => {
  await reaper.lifecycle.ready;
  const capabilities = await reaper.system.getCapabilities(), web = capabilities.webRuntime;
  check(web.crossOriginIsolation.active === true && crossOriginIsolated, 'Cross-origin isolation is not active');
  for (const key of ['sharedArrayBuffer', 'wasmSharedMemory', 'worker', 'blobWorker']) check(web.browser[key] === true, key + ' probe failed');
  const runs = [await runWorkers(false), await runWorkers(true)];
  const workerError = await workerFailure();
  const visits = Number(localStorage.getItem('isolationVisits') || 0) + 1;
  localStorage.setItem('isolationVisits', String(visits));
  const indexedDBVisits = await new Promise((resolve, reject) => {
    const request = indexedDB.open('isolation', 1);
    request.onupgradeneeded = () => request.result.createObjectStore('state');
    request.onerror = () => reject(request.error);
    request.onsuccess = () => {
      const db = request.result, tx = db.transaction('state', 'readwrite'), store = tx.objectStore('state'), get = store.get('visits');
      let count;
      get.onsuccess = () => { count = Number(get.result || 0) + 1; store.put(count, 'visits'); };
      tx.oncomplete = () => { db.close(); resolve(count); };
      tx.onabort = () => { db.close(); reject(tx.error); };
    };
  });
  return { passed: true, version: capabilities.version, origin: location.origin, visits, indexedDBVisits, runs, workerError,
    diagnostics: await reaper.debug.getDiagnostics() };
})();
window.isolationCheck.then(report => {
  document.getElementById('result').textContent = JSON.stringify(report, null, 2);
}, error => { document.getElementById('result').textContent = String(error); });
