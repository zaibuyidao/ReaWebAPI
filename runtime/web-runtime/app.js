import { show, enableDrop } from './ui.js';

const report = { contract: 1, origin: location.origin, userAgent: navigator.userAgent, checks: [] };
const assert = (value, message = 'Unexpected result') => { if (!value) throw new Error(message); };
async function check(name, required, fn) {
  try { await fn(); report.checks.push({ name, required, ok: true }); }
  catch (error) { report.checks.push({ name, required, ok: false, error: String(error) }); }
}
function bounded(promise, milliseconds = 6000) {
  let timer;
  return Promise.race([promise, new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('Timed out')), milliseconds); })])
    .finally(() => clearTimeout(timer));
}
function worker(url, options) {
  const instance = new Worker(new URL(url, import.meta.url), options);
  return bounded(new Promise((resolve, reject) => {
    instance.onmessage = event => event.data.value === 42 ? resolve() : reject(new Error(JSON.stringify(event.data)));
    instance.onerror = event => reject(new Error(event.message));
    instance.postMessage('run');
  })).finally(() => instance.terminate());
}
enableDrop();
async function run() {
await check('HTML / CSS / DOM / Events', true, () => {
  assert(getComputedStyle(document.documentElement).getPropertyValue('--runtime-css').trim() === 'loaded');
  const button = document.createElement('button'); let clicked = false;
  button.addEventListener('click', () => { clicked = true; }); button.click(); assert(clicked);
});
await check('ES modules / dynamic import / Unicode paths', true, async () => {
  assert((await import('./modules/空%20格%23%25.js?check=1')).value === 42);
});
await check('Promise / async / JSON / local fetch', true, async () => {
  const response = await fetch('./data/config.json'); assert(response.ok);
  assert(response.headers.get('content-type').includes('application/json'));
  assert((await response.json()).value === 42);
  assert(!(await fetch('./missing.json')).ok, 'Missing resource should return 404');
});
await check('Timers / requestAnimationFrame', true, async () => {
  await new Promise(resolve => setTimeout(resolve, 1));
  await bounded(new Promise(resolve => { const timer = setInterval(() => { clearInterval(timer); resolve(); }, 1); }));
  report.visibility = document.visibilityState;
  report.viewport = { width: innerWidth, height: innerHeight };
  await bounded(new Promise(resolve => requestAnimationFrame(resolve)));
  assert(innerWidth > 0 && innerHeight > 0, 'Embedded browser has an empty viewport');
});
await check('Canvas 2D', true, () => {
  const context = document.createElement('canvas').getContext('2d'); assert(context);
  context.fillStyle = '#ff0000'; context.fillRect(0, 0, 1, 1);
  assert(context.getImageData(0, 0, 1, 1).data[0] === 255);
});
await check('File / Blob / FileReader / ArrayBuffer', true, async () => {
  const data = Uint8Array.from([0, 128, 255]);
  const file = new File([new Blob([data.buffer])], 'check.bin'); assert(file.size === 3);
  const buffer = await new Promise((resolve, reject) => {
    const reader = new FileReader(); reader.onload = () => resolve(reader.result); reader.onerror = () => reject(reader.error); reader.readAsArrayBuffer(file);
  });
  assert(buffer instanceof ArrayBuffer && new Uint8Array(buffer)[2] === 255);
});
await check('DOM Drag & Drop', true, () => {
  const transfer = new DataTransfer(); transfer.setData('text/plain', 'Drop passed');
  const drop = new DragEvent('drop', { dataTransfer: transfer, cancelable: true });
  document.querySelector('#drop').dispatchEvent(drop);
  assert(drop.defaultPrevented && document.querySelector('#drop-result').textContent === 'Drop passed');
});
await check('localStorage', true, () => {
  report.storageVisits = Number(localStorage.getItem('reaweb-runtime-visits') || 0) + 1;
  localStorage.setItem('reaweb-runtime-visits', String(report.storageVisits));
  assert(Number(localStorage.getItem('reaweb-runtime-visits')) === report.storageVisits);
});
await check('Cookies (profile isolation check)', false, () => {
  const old = document.cookie.match(/(?:^|;\s*)reaweb_runtime_visits=(\d+)/);
  report.cookieVisits = Number(old?.[1] || 0) + 1;
  document.cookie = 'reaweb_runtime_visits=' + report.cookieVisits + '; Path=/; SameSite=Strict; Max-Age=86400';
  assert(document.cookie.includes('reaweb_runtime_visits=' + report.cookieVisits));
});
await check('IndexedDB', false, async () => {
  await bounded(new Promise((resolve, reject) => {
    const request = indexedDB.open('reaweb-runtime-check', 1);
    request.onupgradeneeded = () => request.result.createObjectStore('settings');
    request.onerror = () => reject(request.error);
    request.onsuccess = () => {
      const database = request.result;
      const transaction = database.transaction('settings', 'readwrite');
      const store = transaction.objectStore('settings'); const read = store.get('visits');
      read.onsuccess = () => { report.indexedDBVisits = Number(read.result || 0) + 1; store.put(report.indexedDBVisits, 'visits'); };
      transaction.oncomplete = () => { database.close(); resolve(); };
      transaction.onabort = () => { database.close(); reject(transaction.error); };
    };
  }));
});
await check('Classic Worker + local fetch', false, () => worker('./workers/classic.js'));
await check('Module Worker + import', false, () => worker('./workers/module.js', { type: 'module' }));
await check('WebGL', false, () => {
  const context = document.createElement('canvas').getContext('webgl'); assert(context, 'No GPU/WebGL context');
  context.getExtension('WEBGL_lose_context')?.loseContext();
});
await check('WebSocket constructor', false, () => assert(typeof WebSocket === 'function'));
if (window.reawebProbeEndpoints?.http) await check('Cross-origin HTTP fetch with CORS', false, async () => {
  assert((await (await fetch(window.reawebProbeEndpoints.http)).json()).value === 42);
});
if (window.reawebProbeEndpoints?.websocket) await check('WebSocket round-trip', false, async () => {
  const socket = new WebSocket(window.reawebProbeEndpoints.websocket);
  try { await bounded(new Promise((resolve, reject) => {
    socket.onopen = () => socket.send('echo'); socket.onerror = () => reject(new Error('WebSocket failed'));
    socket.onmessage = event => { assert(event.data === 'echo'); resolve(); };
  })); } finally { socket.close(); }
});
await check('REAPER mirror / main page bridge', true, async () => {
  await reaper.ready;
  report.reaperVersion = await reaper.GetAppVersion();
  report.runtime = (await reaper.ReaWeb_GetCapabilities()).webRuntime;
  assert(report.runtime?.contract === 1 && report.runtime.origin === location.origin);
});
report.passed = report.checks.filter(check => check.required).every(check => check.ok);
show(report);
window.runtimeReport = report;
return report;
}
window.runtimeCheck = run();
