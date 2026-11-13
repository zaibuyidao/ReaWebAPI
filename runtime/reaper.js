(() => {
  'use strict';
  if (window !== window.top || window.reaper) return;
  const pending = new Map();
  const subscriptions = new Map();
  const serviceSubscriptions = new Map();
  const streamConsumers = new Set();
  const latest = new Map();
  const batchReferences = new WeakMap();
  // Event names are NOT listed here on purpose. The native host owns the list and
  // advertises it in the handshake; a second hardcoded copy in this file drifts
  // silently and surfaces as "UNKNOWN_EVENT" in the page at runtime. Ask the host.
  const eventName = async name => {
    const capabilities = await ready;
    if (!Array.isArray(capabilities.events))
      throw failure('SCHEMA_MISMATCH', 'The native host does not advertise events');
    if (!capabilities.events.includes(name)) throw failure('UNKNOWN_EVENT', 'Unknown host event');
    return name;
  };
  // Events that are discrete pushes rather than queryable state. They have no
  // snapshot, so a new listener must not receive the previous value. A frame is
  // replaced every pass, so replaying the last one would show stale numbers.
  const discreteEvents = new Set(['native-drop', 'message', 'file-change', 'native-timer', 'frame']);
  const lifecycleListeners = new Map();
  let cleanupToken = null;
  const documentId = Array.from(crypto.getRandomValues(new Uint32Array(4)), n => n.toString(16)).join('-');
  let sequence = 0, eventSequence = 0, frameSequence = 0, projectEpoch = 0, waiting = 0, closed = false;
  const post = window.chrome?.webview
    ? message => window.chrome.webview.postMessage(message)
    : window.webkit?.messageHandlers?.reaweb
      ? message => window.webkit.messageHandlers.reaweb.postMessage(message)
      : null;
  const failure = (code, message, details) => Object.assign(new Error(message), { code, ...(details ? { details } : {}) });
  const browserBasics = () => {
    const result = { secureContext: window.isSecureContext === true,
      crossOriginIsolated: window.crossOriginIsolated === true, sharedArrayBuffer: false, wasmSharedMemory: false };
    try {
      const view = new Int32Array(new window.SharedArrayBuffer(4));
      Atomics.store(view, 0, 7); result.sharedArrayBuffer = Atomics.load(view, 0) === 7;
    } catch {}
    try { result.wasmSharedMemory = new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true }).buffer instanceof window.SharedArrayBuffer; } catch {}
    return result;
  };
  const isolationRequired = typeof reawebIsolationRequired !== 'undefined' && reawebIsolationRequired;
  const initialBrowser = browserBasics();
  if (isolationRequired && !initialBrowser.crossOriginIsolated) {
    const code = typeof window.crossOriginIsolated === 'boolean' ? 'CROSS_ORIGIN_ISOLATION_FAILED' : 'CROSS_ORIGIN_ISOLATION_UNSUPPORTED';
    try { post?.('__reawebBrowser:' + JSON.stringify({ document: documentId, browser: initialBrowser, error: code })); }
    finally { window.stop(); }
    return;
  }
  let browserProbe;
  const probeBrowser = () => browserProbe ||= (async () => {
    const browser = { ...browserBasics(), worker: null, blobWorker: null, webgl2: null, errors: {}, workers: {} };
    const publish = () => {
      try { post?.('__reawebBrowser:' + JSON.stringify({ document: documentId, browser })); } catch {}
      return browser;
    };
    // Wait for the document's CSP meta elements before testing Worker creation.
    if (window.document?.readyState === 'loading') {
      const loaded = await new Promise(resolve => {
        const done = () => { clearTimeout(timer); resolve(true); };
        const timer = setTimeout(() => { window.removeEventListener('DOMContentLoaded', done); resolve(false); }, 3000);
        window.addEventListener('DOMContentLoaded', done, { once: true });
      });
      if (!loaded) {
        browser.errors.worker = browser.errors.blobWorker = browser.errors.webgl2 = 'DOCUMENT_NOT_READY';
        return publish();
      }
    }
    try {
      const canvas = window.document.createElement('canvas'), gl = canvas.getContext('webgl2');
      browser.webgl2 = !!gl; gl?.getExtension('WEBGL_lose_context')?.loseContext();
    } catch { browser.errors.webgl2 = 'CONTEXT_FAILED'; browser.webgl2 = false; }
    const workerProbe = (url, name) => new Promise(resolve => {
      let worker, timer, memory, done = false;
      const finish = (ok, error, state) => {
        if (done) return; done = true;
        clearTimeout(timer); worker?.terminate();
        browser[name] = ok;
        if (error) browser.errors[name] = error;
        if (state) browser.workers[name] = state;
        resolve();
      };
      try {
        worker = new window.Worker(url);
        const token = documentId + ':' + name;
        worker.onmessage = event => {
          if (event.data?.token !== token) return;
          const state = event.data;
          if (memory && (state.sharedRoundTrip !== true || Atomics.load(new Int32Array(memory.buffer), 0) !== 18))
            return finish(false, 'SHARED_MEMORY_FAILED');
          finish(true, null, { crossOriginIsolated: state.crossOriginIsolated === true,
            sharedArrayBuffer: state.sharedArrayBuffer === true, wasmSharedMemory: state.wasmSharedMemory === true,
            sharedRoundTrip: memory ? true : null });
        };
        worker.onerror = event => { event.preventDefault?.(); finish(false, 'WORKER_LOAD_OR_EXECUTION_FAILED'); };
        worker.onmessageerror = () => finish(false, 'WORKER_MESSAGE_FAILED');
        timer = setTimeout(() => finish(null, 'PROBE_TIMEOUT'), 3000);
        if (browser.wasmSharedMemory) {
          memory = new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true });
          Atomics.store(new Int32Array(memory.buffer), 0, 17);
        }
        worker.postMessage({ token, memory });
      } catch { finish(false, 'WORKER_CREATE_OR_SEND_FAILED'); }
    });
    if (window.location?.protocol === 'reaweb:' || (typeof reawebLocalResources !== 'undefined' && reawebLocalResources)) {
      const url = window.location.origin + '/__reaweb_runtime__/probe.js';
      const blobProbe = async () => {
        let objectUrl;
        const controller = new AbortController(), timeout = setTimeout(() => controller.abort(), 3000);
        try {
          const response = await fetch(url, { signal: controller.signal });
          if (!response.ok) throw failure('PROBE_RESOURCE_FAILED', 'Worker probe resource unavailable');
          const source = await response.text();
          objectUrl = URL.createObjectURL(new Blob([source], { type: 'text/javascript' }));
          await workerProbe(objectUrl, 'blobWorker');
        } catch (error) { browser.errors.blobWorker = error.name === 'AbortError' ? 'PROBE_TIMEOUT' : 'PROBE_RESOURCE_FAILED'; }
        finally { clearTimeout(timeout); if (objectUrl) URL.revokeObjectURL(objectUrl); }
      };
      await Promise.all([workerProbe(url, 'worker'), blobProbe()]);
    } else browser.errors.worker = browser.errors.blobWorker = 'PROBE_RESOURCE_UNAVAILABLE';
    return publish();
  })();
  const health = { timeouts: 0, queueRejected: 0, sendFailures: 0, scriptErrors: 0,
    promiseErrors: 0, callbackErrors: 0, streamErrors: 0, streamCloses: 0, streamPackets: 0, scriptLine: 0, scriptColumn: 0 };
  let diagnosticEnabled = false, timeoutMethod = '';
  const configureHealth = enabled => {
    if (diagnosticEnabled === enabled) return;
    diagnosticEnabled = enabled;
    for (const key of Object.keys(health)) health[key] = 0;
    timeoutMethod = '';
    const now = enabled ? Date.now() : 0;
    for (const item of pending.values()) {
      if (enabled) item.at = now;
      else { delete item.at; delete item.method; }
    }
  };
  const reportHealth = probe => {
    const oldest = pending.values().next().value;
    try { post('__reawebDiagnostic:' + JSON.stringify({ document: documentId, probe, ...health,
      pagePending: pending.size, activeStreams: streamConsumers.size, oldestAgeMs: oldest ? Math.max(0, Date.now() - oldest.at) : 0,
      oldestStarted: oldest?.timer === null ? 1 : 0, method: oldest?.method || '', timeoutMethod })); } catch { if (diagnosticEnabled) ++health.sendFailures; }
  };
  const byteString = value => {
    let binary = '';
    for (let i = 0; i < value.length; i += 8192) binary += String.fromCharCode(...value.subarray(i, i + 8192));
    return btoa(binary);
  };
  const encodeValue = (_, value) => {
    if (batchReferences.has(value))
      throw failure('INVALID_ARGUMENT', 'Batch references can only be used inside their builder');
    if (value instanceof Uint8Array) return { __reawebBytes: byteString(value) };
    if (value instanceof Float64Array) {
      const data = new Uint8Array(value.length * 8), view = new DataView(data.buffer);
      for (let i = 0; i < value.length; ++i) view.setFloat64(i * 8, value[i], true);
      return { __reawebFloat64: byteString(data) };
    }
    if (typeof value === 'number' && !Number.isFinite(value))
      throw failure('INVALID_ARGUMENT', 'NaN and Infinity are not native API arguments');
    return value;
  };
  const decodeValue = value => {
    if (value instanceof Uint8Array) return value;
    if (value && typeof value === 'object' && typeof value.__reawebBytes === 'string') {
      const binary = atob(value.__reawebBytes);
      return Uint8Array.from(binary, c => c.charCodeAt(0));
    }
    if (Array.isArray(value)) return value.map(decodeValue);
    if (value && typeof value === 'object') return Object.fromEntries(Object.entries(value).map(([key, child]) => [key, decodeValue(child)]));
    return value;
  };
  const notify = (callback, data) => {
    try { Promise.resolve(callback(data)).catch(error => { if (diagnosticEnabled) ++health.callbackErrors; console.error('[ReaWebAPI event]', error); }); }
    catch (error) { if (diagnosticEnabled) ++health.callbackErrors; console.error('[ReaWebAPI event]', error); }
  };
  const receive = message => {
    if (closed || message?.document !== documentId) return;
    if (typeof message.diagnosticEnabled === 'boolean') { configureHealth(message.diagnosticEnabled); return; }
    if (Number.isSafeInteger(message.diagnosticProbe) && message.diagnosticProbe > 0) {
      if (diagnosticEnabled) reportHealth(message.diagnosticProbe);
      return;
    }
    if (message.lifecycle) { runCleanup(message.lifecycle); return; }
    if (message.serviceEvent) {
      const entries = serviceSubscriptions.get(message.service);
      if (entries) for (const [name, entry] of [...entries]) {
        if (entry.handle !== message.serviceHandle) continue;
        if (name === message.serviceEvent) for (const listener of [...entry.listeners]) notify(listener, message.data);
        if (message.serviceEvent === 'unloaded') {
          for (const listener of entry.listeners) listener.invalidate();
          entries.delete(name);
        }
      }
      return;
    }
    if (message.event) {
      if (!Number.isSafeInteger(message.sequence)) return;
      // Frames may bypass older reliable output. A late frame must neither
      // replace a newer frame nor suppress an ordered message/event.
      if (message.event === 'frame') {
        if (message.sequence <= frameSequence) return;
        frameSequence = message.sequence;
      } else {
        if (message.sequence <= eventSequence) return;
        eventSequence = message.sequence;
      }
      if (message.event === 'projectchange') projectEpoch = message.data.projectEpoch;
      const entry = subscriptions.get(message.event);
      if (entry) {
        if (!discreteEvents.has(message.event)) entry.last = message.data;
        for (const callback of [...entry.listeners]) notify(callback, message.data);
      }
      return;
    }
    const item = pending.get(message?.id);
    if (!item) return;
    // Native dialogs and renders can legitimately outlive the queue timeout.
    if (message.started === true) { clearTimeout(item.timer); item.timer = null; return; }
    pending.delete(message.id);
    clearTimeout(item.timer);
    if (message.error) {
      if (message.error.code === 'PROJECT_CHANGED' && message.error.details?.projectEpoch)
        projectEpoch = message.error.details.projectEpoch;
      item.reject(failure(message.error.code, message.error.message, message.error.details));
    } else item.resolve(item.service ? message.result : decodeValue(message.result));
  };
  Object.defineProperty(window, '__reawebReceive', { value: receive });
  const send = (method, args) => new Promise((resolve, reject) => {
    if (closed) return reject(failure('WINDOW_CLOSED', 'The WebView document was closed'));
    if (!post) return reject(failure('NO_RUNTIME', 'Open this page with ReaWebAPI inside REAPER'));
    if (pending.size >= 256) { if (diagnosticEnabled) ++health.queueRejected; return reject(failure('QUEUE_LIMIT', 'Too many pending API calls')); }
    const id = ++sequence;
    const timer = setTimeout(() => {
      pending.delete(id);
      if (diagnosticEnabled) { ++health.timeouts; timeoutMethod = method; }
      reject(failure('TIMEOUT', `${method} timed out; check the tool state before repeating a write`));
    }, 30000);
    const item = { resolve, reject, timer, service: method === 'ReaWeb_ServiceInvoke' };
    if (diagnosticEnabled) { item.method = method; item.at = Date.now(); }
    pending.set(id, item);
    try {
      const message = JSON.stringify({ id, document: documentId, project: projectEpoch, expiresAt: Date.now() + 25000, method, args,
        ...(method === '__reawebHello' ? { browser: initialBrowser } : {}) }, encodeValue);
      if (new TextEncoder().encode(message).length > 64 * 1024 * 1024) throw failure('MESSAGE_LIMIT', 'Bridge message exceeds 64 MiB');
      post(message);
    } catch (error) {
      if (diagnosticEnabled) ++health.sendFailures;
      clearTimeout(timer);
      pending.delete(id);
      reject(error);
    }
  });
  const ready = send('__reawebHello', [1]).then(capabilities => {
    if (capabilities?.protocol !== 1) throw failure('PROTOCOL_MISMATCH', 'Unsupported native bridge protocol');
    if (!Array.isArray(capabilities.methods) || reawebApiMethods.some(name => !capabilities.methods.includes(name)))
      throw failure('SCHEMA_MISMATCH', 'JavaScript API definitions do not match the native host');
    projectEpoch = capabilities.projectEpoch;
    return Object.freeze(capabilities);
  });
  // Loading a page that does not use the API should not create an unhandled rejection.
  ready.catch(() => {});
  if (isolationRequired) ready.then(probeBrowser).catch(() => {});
  const call = (method, args) => {
    if (closed) return Promise.reject(failure('WINDOW_CLOSED', 'The WebView document was closed'));
    if (waiting >= 256) { if (diagnosticEnabled) ++health.queueRejected; return Promise.reject(failure('QUEUE_LIMIT', 'Too many pending API calls')); }
    ++waiting;
    return ready.then(() => send(method, args)).finally(() => --waiting);
  };
  let stopTitle = () => {};
  const startTitle = () => {
    const document = window.document;
    if (closed || typeof document?.title !== 'string' || !window.MutationObserver) return;
    let selected, timer, stopped = false;
    const update = async () => {
      if (closed || stopped) return;
      const title = document.title.replace(/\0/g, '').trim();
      if (title === selected) return;
      selected = title;
      try {
        if (!await call('ReaWeb_DocumentTitle', [title])) stopTitle();
      } catch (error) {
        if (!closed && !stopped) console.warn('[ReaWebAPI title]', error);
      }
    };
    const schedule = () => { clearTimeout(timer); timer = setTimeout(update, 0); };
    const relevant = node => node.nodeType === 1 && (['TITLE', 'HEAD'].includes(node.tagName) || node.querySelector('title,head'));
    const observer = new window.MutationObserver(records => {
      if (records.some(record => record.target.tagName === 'TITLE' || record.target.parentNode?.tagName === 'TITLE' ||
        (record.type === 'childList' && [...record.addedNodes, ...record.removedNodes].some(relevant)))) schedule();
    });
    observer.observe(document, { subtree: true, childList: true, characterData: true });
    stopTitle = () => { stopped = true; clearTimeout(timer); observer.disconnect(); };
    void update();
  };
  let stopFavicon = () => {};
  const startFavicon = () => {
    const document = window.document;
    if (closed || !document?.querySelectorAll || !window.MutationObserver || !window.fetch) return;
    const formats = { 'image/png': '.png', 'image/svg+xml': '.svg', 'image/x-icon': '.ico', 'image/vnd.microsoft.icon': '.ico' };
    const limit = 4 * 1024 * 1024;
    let revision = 0, selected, timer, controller, stopped = false;
    let mediaListeners = [];
    const releaseMedia = () => {
      for (const [query, listener] of mediaListeners) query.removeEventListener('change', listener);
      mediaListeners = [];
    };
    const readIcon = async (response, signal) => {
      if (!response.ok) throw new Error(`Favicon HTTP ${response.status}`);
      if (Number(response.headers.get('content-length')) > limit) throw new Error('Favicon exceeds 4 MiB');
      const reader = response.body?.getReader();
      if (!reader) throw new Error('Favicon response has no readable body');
      const chunks = [];
      let length = 0;
      try {
        for (;;) {
          const { done, value } = await reader.read();
          if (signal.aborted) throw new DOMException('Favicon request aborted', 'AbortError');
          if (done) break;
          length += value.byteLength;
          if (length > limit) { await reader.cancel(); throw new Error('Favicon exceeds 4 MiB'); }
          chunks.push(value);
        }
      } finally { reader.releaseLock(); }
      const bytes = new Uint8Array(length);
      let offset = 0;
      for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.byteLength; }
      return bytes;
    };
    const sync = async (icon, current, signal) => {
      let timeout, timedOut = false;
      try {
        // Invalidate an earlier native decode before fetching the new resource.
        if (!await call('ReaWeb_Favicon', [{ revision: current }])) { if (current === revision) stopFavicon(); return; }
        if (signal.aborted || current !== revision) return;
        let data = null;
        if (icon) {
          const active = controller;
          timeout = setTimeout(() => { timedOut = true; active.abort(); }, 15000);
          const response = await window.fetch(icon.url, { signal });
          data = { format: icon.format, bytes: await readIcon(response, signal) };
          clearTimeout(timeout);
        }
        if (signal.aborted || current !== revision) return;
        if (!await call('ReaWeb_Favicon', [{ revision: current, icon: data }]) && current === revision) stopFavicon();
      } catch (error) {
        if (current === revision) controller?.abort();
        if (!closed && !stopped && current === revision && (timedOut || error.name !== 'AbortError') && error.code !== 'ICON_SUPERSEDED')
          console.warn('[ReaWebAPI favicon]', timedOut ? new Error('Favicon request timed out') : error);
      } finally { clearTimeout(timeout); }
    };
    const update = () => {
      if (closed || stopped) return;
      releaseMedia();
      let icon = null;
      for (const link of document.head?.querySelectorAll('link[rel][href]') || []) {
        if (!link.rel.toLowerCase().split(/\s+/).includes('icon') || !link.getAttribute('href')?.trim()) continue;
        if (link.media && window.matchMedia) {
          const query = window.matchMedia(link.media);
          query.addEventListener('change', schedule);
          mediaListeners.push([query, schedule]);
          if (!query.matches) continue;
        }
        try {
          const url = new window.URL(link.getAttribute('href'), document.baseURI);
          const type = (link.type || (url.protocol === 'data:' ? url.pathname.split(/[;,]/, 1)[0] : '')).split(';', 1)[0].trim().toLowerCase();
          const format = type ? (Object.hasOwn(formats, type) ? formats[type] : null) : url.pathname.toLowerCase().match(/\.(png|ico|svg)$/)?.[0];
          if (format && ['reaweb:', 'http:', 'https:', 'data:', 'blob:'].includes(url.protocol)) icon = { url: url.href, format };
        } catch { /* Ignore invalid favicon URLs. */ }
      }
      const key = icon ? `${icon.format}:${icon.url}` : '';
      if (key === selected) return;
      const initial = selected === undefined;
      selected = key;
      if (initial && !icon) return;
      controller?.abort();
      controller = new window.AbortController();
      void sync(icon, ++revision, controller.signal);
    };
    const schedule = () => { clearTimeout(timer); timer = setTimeout(update, 0); };
    const relevant = node => node.nodeType === 1 && (['LINK', 'BASE', 'HEAD'].includes(node.tagName) || node.querySelector('link,base,head'));
    const observer = new window.MutationObserver(records => {
      if (records.some(record => record.type === 'attributes' ? ['LINK', 'BASE'].includes(record.target.tagName)
        : [...record.addedNodes, ...record.removedNodes].some(relevant))) schedule();
    });
    observer.observe(document, { subtree: true, childList: true, attributes: true, attributeFilter: ['href', 'rel', 'type', 'media', 'sizes'] });
    stopFavicon = () => { stopped = true; clearTimeout(timer); controller?.abort(); observer.disconnect(); releaseMedia(); };
    update();
  };
  const api = Object.create(null);
  // Only the official REAPER mirror is exposed at the root. ReaWeb_* strings
  // below are private transport commands, never public JavaScript aliases.
  const host = name => (...args) => call(name, args);
  for (const name of reawebApiMethods) api[name] = (...args) => call(name, args).then(result => {
    if (result?.__reawebCall === true) {
      for (const update of result.arrays) {
        const target = args[update.index];
        const data = decodeValue(update.values);
        const binary = data instanceof Uint8Array;
        const length = binary ? data.byteLength / 8 : data.length;
        if (!target || !Number.isInteger(length) || target.length !== length)
          throw failure('ARRAY_CHANGED', 'Keep sample buffers the same size until the API call resolves');
        const view = binary ? new DataView(data.buffer, data.byteOffset, data.byteLength) : null;
        for (let i = 0; i < length; ++i) target[i] = view ? view.getFloat64(i * 8, true) : data[i];
      }
      result = decodeValue(result.value);
    }
    return reawebApiVoidMethods.includes(name) ? undefined : result;
  });
  const onEvent = async (name, callback) => {
    await eventName(name);
    if (typeof callback !== 'function') throw failure('INVALID_ARGUMENT', 'Expected an event callback');
    let entry = subscriptions.get(name);
    if (!entry) {
      entry = { listeners: new Set(), initial: call('ReaWeb_Subscribe', [name]), last: null };
      subscriptions.set(name, entry);
    }
    // Preserve callback identity while retaining independent disposer handles.
    let disposed = false;
    const listener = data => { if (!disposed) return callback(data); };
    listener.callback = callback;
    const dispose = async () => {
      if (disposed) return;
      disposed = true;
      entry.listeners.delete(listener);
      if (!entry.listeners.size && subscriptions.get(name) === entry) {
        subscriptions.delete(name);
        if (!closed) await call('ReaWeb_Unsubscribe', [name]);
      }
    };
    listener.dispose = dispose;
    entry.listeners.add(listener);
    try {
      const initial = await entry.initial;
      if (closed) throw failure('WINDOW_CLOSED', 'The WebView document was closed');
      if (!disposed && !discreteEvents.has(name) && (entry.last ?? initial)) notify(listener, entry.last ?? initial);
    } catch (error) {
      disposed = true;
      entry.listeners.delete(listener);
      if (!entry.listeners.size && subscriptions.get(name) === entry) subscriptions.delete(name);
      throw error;
    }
    return dispose;
  };
  const offEvent = async (name, callback) => {
    await eventName(name);
    if (typeof callback !== 'function') throw failure('INVALID_ARGUMENT', 'Expected an event callback');
    const listeners = subscriptions.get(name)?.listeners;
    if (listeners) await Promise.all([...listeners].filter(listener => listener.callback === callback).map(listener => listener.dispose()));
  };
  const serviceName = name => {
    if (typeof name !== 'string' || !/^[A-Za-z0-9_.-]{1,128}$/.test(name))
      throw failure('INVALID_ARGUMENT', 'Expected 1..128 ASCII letters, digits, _, -, .');
    return name;
  };
  const service = name => {
    serviceName(name);
    const entries = () => {
      if (!serviceSubscriptions.has(name)) serviceSubscriptions.set(name, new Map());
      return serviceSubscriptions.get(name);
    };
    return Object.freeze({
      invoke: async (method, payload = null) => call('ReaWeb_ServiceInvoke', [name, serviceName(method), payload]),
      send: (method, payload = null) => {
        call('ReaWeb_ServiceSend', [name, serviceName(method), payload]).catch(error => console.error('[ReaWebAPI service]', error));
      },
      on: async (event, callback) => {
        serviceName(event);
        if (typeof callback !== 'function') throw failure('INVALID_ARGUMENT', 'Expected an event callback');
        const map = entries();
        let entry = map.get(event);
        if (!entry) {
          entry = { listeners: new Set(), handle: null };
          entry.initial = call('ReaWeb_ServiceSubscribe', [name, event]).then(result => { entry.handle = result.handle; });
          map.set(event, entry);
        }
        let disposed = false;
        const listener = data => { if (!disposed) return callback(data); };
        listener.callback = callback;
        listener.invalidate = () => { disposed = true; };
        listener.dispose = async () => {
          if (disposed) return;
          disposed = true; entry.listeners.delete(listener);
          if (!entry.listeners.size && map.get(event) === entry) {
            map.delete(event);
            if (!closed) await call('ReaWeb_ServiceUnsubscribe', [name, event]);
          }
        };
        entry.listeners.add(listener);
        try {
          await entry.initial;
          if (closed) throw failure('WINDOW_CLOSED', 'The WebView document was closed');
        } catch (error) {
          disposed = true; entry.listeners.delete(listener);
          if (!entry.listeners.size && map.get(event) === entry) map.delete(event);
          throw error;
        }
        return listener.dispose;
      },
      off: async (event, callback) => {
        serviceName(event);
        if (typeof callback !== 'function') throw failure('INVALID_ARGUMENT', 'Expected an event callback');
        const listeners = serviceSubscriptions.get(name)?.get(event)?.listeners;
        if (listeners) await Promise.all([...listeners].filter(item => item.callback === callback).map(item => item.dispose()));
      }
    });
  };
  const withUndo = async (label, callback) => {
    if (typeof callback !== 'function') throw failure('INVALID_ARGUMENT', 'Expected an Undo callback');
    const token = await call('ReaWeb_BeginUndo', [label]);
    let failed = false;
    try { return await callback(); }
    catch (error) { failed = true; throw error; }
    finally {
      try { await call('ReaWeb_EndUndo', [token]); }
      catch (error) { if (!failed) throw error; }
    }
  };
  const batch = async (input, ...options) => {
    if (typeof input !== 'function') return call('ReaWeb_Batch', [input, ...options]);
    const capabilities = await ready;
    if (closed) throw failure('WINDOW_CLOSED', 'The WebView document was closed');
    if (!Array.isArray(capabilities.batchMethods))
      throw failure('SCHEMA_MISMATCH', 'The native host does not advertise batch methods');
    const allowed = new Set(capabilities.batchMethods.filter(name => reawebApiMethods.includes(name)));
    const owner = {}, calls = [];
    let active = true;
    const invalid = message => { throw failure('INVALID_ARGUMENT', message); };
    const reference = (index, tupleSize = 0, path) => {
      const value = tupleSize ? Array.from({ length: tupleSize }, (_, i) => reference(index, 0, [i])) : {};
      batchReferences.set(value, { owner, wire: path ? { $ref: index, path } : { $ref: index } });
      Object.defineProperties(value, {
        then: { get: () => invalid('Batch references cannot be awaited; use a synchronous callback') },
        [Symbol.toPrimitive]: { value: () => invalid('Batch references are not resolved values') }
      });
      return Object.freeze(value);
    };
    const ownedReference = value => {
      const ref = batchReferences.get(value);
      if (ref && ref.owner !== owner) invalid('Batch references cannot cross builders');
      return ref;
    };
    // Snapshot literals and return structures before dispatch. Only top-level
    // native arguments support references in the existing batch protocol.
    const visit = (value, onReference, ancestors = new Set()) => {
      const ref = ownedReference(value);
      if (ref) return onReference(ref.wire, value);
      if (value === null || value === undefined || ['string', 'boolean'].includes(typeof value)) return value;
      if (typeof value === 'number' && Number.isFinite(value)) return value;
      if (typeof value !== 'object') return invalid('Expected a batch literal, array, object or reference');
      if (typeof value.then === 'function') {
        Promise.resolve(value).catch(() => {});
        invalid('Batch callbacks and return values must be synchronous');
      }
      if (value instanceof Uint8Array) return value.slice();
      const prototype = Object.getPrototypeOf(value);
      if (!Array.isArray(value) && prototype !== null && Object.getPrototypeOf(prototype) !== null)
        invalid('Batch structures must contain plain objects or arrays');
      if (ancestors.has(value)) invalid('Batch structures cannot contain cycles');
      ancestors.add(value);
      try {
        return Array.isArray(value) ? value.map(child => visit(child, onReference, ancestors))
          : Object.fromEntries(Object.entries(value).map(([key, child]) => [key, visit(child, onReference, ancestors)]));
      } finally { ancestors.delete(value); }
    };
    const methods = Object.create(null);
    for (const name of allowed) methods[name] = (...args) => {
      if (!active) invalid('The batch builder callback has finished');
      if (calls.length >= 128) invalid('A batch must contain 1 to 128 calls');
      const encoded = args.map(value => {
        const ref = ownedReference(value);
        if (ref) return ref.wire;
        const literal = visit(value, () => invalid('Batch argument references must be top-level'));
        if (literal && typeof literal === 'object' && Object.hasOwn(literal, '$ref'))
          invalid('Use builder references instead of literal $ref objects');
        return literal;
      });
      const index = calls.length;
      calls.push({ method: name, args: encoded });
      return reference(index, reawebApiTupleSizes[name] || 0);
    };
    const builder = new Proxy(Object.freeze(methods), {
      get: (target, name) => {
        if (Object.hasOwn(target, name)) return target[name];
        return invalid(`This API is not batchable: ${String(name)}`);
      }
    });
    let returned;
    try { returned = input(builder); }
    finally { active = false; }
    const projection = visit(returned, (_, value) => value);
    if (!calls.length) invalid('A batch must contain 1 to 128 calls');
    const results = await call('ReaWeb_Batch', [calls, ...options]);
    if (returned === undefined) return results;
    return visit(projection, wire => {
      let value = results[wire.$ref];
      for (const key of wire.path || []) value = value[key];
      return value;
    });
  };
  const setTrackValueLatest = (track, key, value) => new Promise((resolve, reject) => {
    if (closed) return reject(failure('WINDOW_CLOSED', 'The WebView document was closed'));
    if (!track || track.type !== 'MediaTrack' || typeof track.id !== 'string' ||
        !['D_VOL', 'D_PAN', 'B_MUTE', 'I_SOLO'].includes(key) || !Number.isFinite(value))
      return reject(failure('INVALID_ARGUMENT', 'Expected a track handle, supported key and finite value'));
    const token = JSON.stringify([track.id, key]);
    let entry = latest.get(token);
    const request = { track, key, value, resolve, reject };
    if (entry) {
      if (entry.next) entry.next.resolve({ applied: false, superseded: true });
      entry.next = request;
      return;
    }
    if (latest.size >= 128) return reject(failure('QUEUE_LIMIT', 'Too many continuous controls'));
    entry = { next: null };
    latest.set(token, entry);
    const drain = async first => {
      let current = first;
      while (current) {
        try {
          const applied = await call('SetMediaTrackInfo_Value', [current.track, current.key, current.value]);
          current.resolve({ applied, superseded: false });
        } catch (error) { current.reject(error); }
        current = entry.next;
        entry.next = null;
      }
      latest.delete(token);
    };
    drain(request);
  });
  const sharedVideo = new Map();
  const nativeBuffers = window.chrome?.webview;
  if (nativeBuffers?.releaseBuffer) nativeBuffers.addEventListener('sharedbufferreceived', event => {
    const token = event.additionalData?.reawebVideo;
    if (typeof token !== 'string') return;
    const buffer = event.getBuffer();
    if (closed) { nativeBuffers.releaseBuffer(buffer); return; }
    const waiting = sharedVideo.get(token);
    if (waiting?.resolve) { sharedVideo.delete(token); clearTimeout(waiting.timer); waiting.resolve(buffer); }
    else {
      if (waiting) { clearTimeout(waiting.timer); nativeBuffers.releaseBuffer(waiting.buffer); }
      const timer = setTimeout(() => { sharedVideo.delete(token); nativeBuffers.releaseBuffer(buffer); }, 10000);
      sharedVideo.set(token, { buffer, timer });
    }
  });
  const takeVideoBuffer = token => {
    const pending = sharedVideo.get(token);
    if (pending?.buffer) { sharedVideo.delete(token); clearTimeout(pending.timer); return Promise.resolve(pending.buffer); }
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { sharedVideo.delete(token); reject(failure('TIMEOUT', 'Video memory mapping timed out')); }, 10000);
      sharedVideo.set(token, { resolve, timer });
    });
  };
  const openStream = async name => {
    if (typeof name !== 'string' || !/^[A-Za-z0-9_.-]{1,128}$/.test(name)) throw failure('INVALID_ARGUMENT', 'Expected a native stream name');
    const descriptor = await call('ReaWeb_StreamOpen', [name]);
    const nativeVideoCopy = descriptor.videoNativeCopy ? window.__reawebVideoCopy : null;
    let socket, videoBuffer, active = true, last = null, status = null, consumerDrops = 0;
    const videoRequest = descriptor.videoResource ? new AbortController() : null;
    const listeners = new Map(), queue = [];
    const ordered = ['audio', 'midi'].includes(descriptor.kind);
    const announce = (event, data) => { for (const listener of listeners.get(event) || []) notify(listener, data); };
    const finish = (code, message) => {
      if (typeof nativeVideoCopy === 'function') nativeVideoCopy(descriptor.token, -2);
      videoRequest?.abort();
      if (videoBuffer) { nativeBuffers.releaseBuffer(videoBuffer); videoBuffer = null; }
      if (!active) return;
      active = false; status = failure(code, message || code); queue.length = 0; last = null;
      streamConsumers.delete(consumer); announce('close', status); listeners.clear();
    };
    const consumer = Object.freeze({
      info: Object.freeze(Object.fromEntries(Object.entries(descriptor).filter(([key]) => !['token', 'url', 'sharedVideo', 'videoResource', 'videoNativeCopy'].includes(key)))),
      latest: () => last,
      read: () => ordered ? queue.shift() || null : last,
      get closed() { return !active; },
      get error() { return status; },
      get dropped() { return consumerDrops; },
      on: (event, callback) => {
        if (!['data', 'close', 'error'].includes(event) || typeof callback !== 'function') throw failure('INVALID_ARGUMENT', 'Expected data, close or error and a callback');
        if (!active) { if (event === 'close') notify(callback, status); return () => {}; }
        if (!listeners.has(event)) listeners.set(event, new Set());
        listeners.get(event).add(callback); return () => listeners.get(event)?.delete(callback);
      },
      close: async () => {
        if (!active) return;
        finish('STREAM_CLOSED', 'Consumer detached'); socket?.close();
        if (!closed) await call('ReaWeb_StreamDetach', [descriptor.token]);
      }
    });
    streamConsumers.add(consumer);
    try {
      if (descriptor.videoNativeCopy) {
        if (typeof nativeVideoCopy !== 'function') throw failure('TRANSPORT_ERROR', 'Native video mapping unavailable');
        const deadline = Date.now() + 10000;
        while (active && nativeVideoCopy(descriptor.token, -1) === null && Date.now() < deadline)
          await new Promise(resolve => setTimeout(resolve, 10));
        if (!active) throw status || failure('STREAM_CLOSED', 'Video stream closed');
        if (nativeVideoCopy(descriptor.token, -1) === null) throw failure('TIMEOUT', 'Native video mapping timed out');
        if (!descriptor.sharedVideo || descriptor.kind !== 'frame' || !['rgba8', 'i420'].includes(descriptor.format) || nativeVideoCopy(descriptor.token, -1) !== descriptor.maxBytes)
          throw failure('UNSUPPORTED_FORMAT', 'Invalid native video mapping');
      } else if (descriptor.sharedVideo && !descriptor.videoResource) {
        videoBuffer = await takeVideoBuffer(descriptor.token);
        if (!active || descriptor.kind !== 'frame' || !['rgba8', 'i420'].includes(descriptor.format) || videoBuffer.byteLength !== descriptor.maxBytes * 2)
          throw failure('UNSUPPORTED_FORMAT', 'Invalid shared video buffer');
      }
      await new Promise((resolve, reject) => {
        socket = new WebSocket(descriptor.url); socket.binaryType = 'arraybuffer';
        const timeout = setTimeout(() => { socket.close(); reject(failure('TIMEOUT', 'Stream connection timed out')); }, 10000);
        let connected = false;
        socket.onopen = () => { connected = true; clearTimeout(timeout); resolve(); };
        socket.onerror = () => {
          if (diagnosticEnabled) ++health.streamErrors;
          const error = failure('TRANSPORT_ERROR', 'Native stream connection failed'); announce('error', error);
          if (!connected) { clearTimeout(timeout); reject(error); }
        };
        socket.onclose = event => {
          if (diagnosticEnabled && active && event.code !== 1000) ++health.streamCloses;
          clearTimeout(timeout);
          const code = ['STREAM_CLOSED', 'EXTENSION_UNLOADED', 'TIMEOUT', 'UNSUPPORTED_FORMAT', 'NATIVE_ERROR'].includes(event.reason) ? event.reason : 'TRANSPORT_ERROR';
          finish(code, event.reason || 'Native stream connection closed');
          if (!connected) reject(status || failure(code, code));
        };
        let videoReads = Promise.resolve();
        const packetError = error => { if (!active) return; if (diagnosticEnabled) ++health.streamErrors; announce('error', error); finish(error.code || 'UNSUPPORTED_FORMAT', error.message); socket.close(); };
        const receive = (event, resourceBytes) => {
          if (!active) return;
          try {
            const buffer = event.data, view = new DataView(buffer);
            const version = descriptor.layoutVersion === 3 ? 3 : descriptor.layoutVersion === 2 ? 2 : 1;
            const header = version === 3 ? 56 : version === 2 ? 80 : 40;
            const shared = !!descriptor.sharedVideo;
            const size = buffer.byteLength >= header ? Number(view.getBigUint64(32, true)) : 0;
            if (buffer.byteLength < header || view.getUint32(0, true) !== (version << 24 | 0x535752) ||
                view.getUint8(4) !== ['', 'frame', 'audio', 'spectrum', 'meter', 'waveform', 'binary', 'midi'].indexOf(descriptor.kind) ||
                !Number.isSafeInteger(size) || size < 0 || size > descriptor.maxBytes ||
                (shared ? buffer.byteLength !== header + 4 || !(view.getUint8(5) & 2) || view.getUint32(header, true) > 1 ||
                  (version !== 3 && size !== descriptor.maxBytes) : size !== buffer.byteLength - header))
              throw failure('UNSUPPORTED_FORMAT', 'Invalid stream packet');
            const width = version === 3 ? view.getUint32(40, true) : descriptor.width;
            const height = version === 3 ? view.getUint32(44, true) : descriptor.height;
            const stride = version === 3 ? view.getUint32(48, true) : descriptor.stride;
            const planar = descriptor.format === 'i420';
            if (version === 3 && (!descriptor.dynamicResolution || descriptor.kind !== 'frame' ||
                !!(view.getUint8(5) & 2) !== shared ||
                !['rgba8', 'i420'].includes(descriptor.format) || width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
                width * height * 4 > 40 * 1024 * 1024 ||
                stride !== width * (planar ? 1 : 4) || (planar && (width % 2 || height % 2)) ||
                size !== stride * height * (planar ? 1.5 : 1) || size > 40 * 1024 * 1024))
              throw failure('UNSUPPORTED_FORMAT', 'Invalid video dimensions');
            const sequence = view.getBigUint64(8, true), timestamp = view.getFloat64(16, true), dropped = view.getBigUint64(24, true);
            // Copy before ACK so retained packets stay immutable when the host reuses a slot.
            let bytes;
            if (descriptor.videoResource && !resourceBytes) {
              return fetch(descriptor.videoResource + view.getUint32(header, true), { cache: 'no-store', signal: videoRequest.signal })
                .then(response => { if (!response.ok) throw failure('TRANSPORT_ERROR', 'Video resource unavailable'); return response.arrayBuffer(); })
                .then(buffer => receive(event, new Uint8Array(buffer))).catch(packetError);
            }
            if (descriptor.videoNativeCopy) {
              const slot = view.getUint32(header, true);
              const copy = version === 3 ? nativeVideoCopy(descriptor.token, slot, size) : nativeVideoCopy(descriptor.token, slot);
              if (!(copy instanceof ArrayBuffer) || copy.byteLength !== size) throw failure('UNSUPPORTED_FORMAT', 'Invalid native video frame');
              bytes = new Uint8Array(copy);
            } else if (descriptor.videoResource) {
              bytes = resourceBytes;
              if (bytes.byteLength !== size) throw failure('UNSUPPORTED_FORMAT', 'Invalid video resource size');
            } else bytes = shared ? new Uint8Array(videoBuffer, view.getUint32(header, true) * descriptor.maxBytes, size).slice() : new Uint8Array(buffer, header);
            const data = descriptor.format === 'float32' ? new Float32Array(buffer, header) : bytes;
            const timing = version === 2 ? Object.freeze({ clockEpoch: view.getBigUint64(40, true),
              firstSample: view.getBigUint64(48, true), sourceDropped: view.getBigUint64(56, true),
              position: view.getFloat64(64, true), playState: view.getUint8(72), discontinuity: !!view.getUint8(73) }) : undefined;
            const packet = Object.freeze({ sequence, frameId: sequence, timestamp, producerDropped: dropped, data, bytes,
              ...(timing ? { timing } : {}),
              ...(descriptor.kind === 'frame' ? { width, height, stride } : {}),
              ...(descriptor.kind === 'frame' && (view.getUint8(5) & 1) ? { discontinuity: true } : {}) });
            last = packet;
            if (diagnosticEnabled) ++health.streamPackets;
            if (ordered) { if (queue.length < descriptor.capacity) queue.push(packet); else ++consumerDrops; }
            announce('data', packet);
            socket.send(new Uint8Array([1]));
          } catch (error) { packetError(error); }
        };
        socket.onmessage = event => {
          if (descriptor.videoResource) videoReads = videoReads.then(() => receive(event));
          else receive(event);
        };
      });
      return consumer;
    } catch (error) {
      finish(error.code || 'TRANSPORT_ERROR', error.message);
      socket?.close(); if (!closed) await call('ReaWeb_StreamDetach', [descriptor.token]).catch(() => {});
      throw error;
    }
  };
  api.stream = Object.freeze({ open: openStream, getDiagnostics: () => call('ReaWeb_StreamDiagnostics', []) });
  const videoConsumers = new Set();
  const videoWorkers = new WeakSet();
  const openVideoSurface = async (options = {}) => {
    if (globalThis.document?.hidden) throw failure('VIDEO_PAGE_HIDDEN', 'Show the page before opening project video');
    let descriptor, active = true, status = null, closing, released = false;
    const listeners = new Map();
    const announce = (event, value) => { for (const callback of listeners.get(event) || []) notify(callback, value); };
    const release = () => {
      if (typeof descriptor?.token !== 'string' || released || closed) return Promise.resolve();
      released = true;
      return call('ReaWeb_VideoSurfaceClose', [descriptor.token]).then(() => {});
    };
    const stop = (error = failure('STREAM_CLOSED', 'Video surface detached')) => {
      if (active) {
        active = false; status = error;
        streamConsumers.delete(surface); videoConsumers.delete(surface);
        if (error.code !== 'STREAM_CLOSED') announce('error', error);
        announce('close', error); listeners.clear();
      }
      return closing || (closing = release());
    };
    const invoke = async (method, args) => {
      if (!active) throw status;
      try { return await call(method, [descriptor.token, ...args]); }
      catch (error) {
        if (['STREAM_CLOSED', 'STREAM_NOT_FOUND', 'VIDEO_SURFACE_CLOSED', 'VIDEO_PROJECT_CHANGED', 'WINDOW_CLOSED'].includes(error.code))
          await stop(error).catch(() => {});
        else announce('error', error);
        throw error;
      }
    };
    const surface = Object.freeze({
      get info() { return Object.freeze(Object.fromEntries(Object.entries(descriptor || {}).filter(([key]) => key !== 'token' && key !== 'url'))); },
      readablePixels: false,
      get closed() { return !active; }, get error() { return status; },
      on: (event, callback) => {
        if (!['close', 'error'].includes(event) || typeof callback !== 'function') throw failure('INVALID_ARGUMENT', 'Expected close or error and a callback');
        if (!active) { if (event === 'close') notify(callback, status); return () => {}; }
        if (!listeners.has(event)) listeners.set(event, new Set());
        listeners.get(event).add(callback); return () => listeners.get(event)?.delete(callback);
      },
      setLayout: layout => invoke('ReaWeb_VideoSurfaceLayout', [layout]),
      getDiagnostics: async () => {
        const result = await invoke('ReaWeb_VideoSurfaceDiagnostics', []);
        if (result.closed) await stop(failure(result.errorCode || (result.error ? 'VIDEO_RENDERER_UNAVAILABLE' : 'STREAM_CLOSED'), result.error || 'Video surface closed')).catch(() => {});
        return result;
      },
      close: () => stop()
    });
    streamConsumers.add(surface); videoConsumers.add(surface);
    try {
      descriptor = await call('ReaWeb_VideoSurfaceOpen', [options]);
      if (!descriptor || typeof descriptor.token !== 'string' || !['native-metal', 'native-d3d11'].includes(descriptor.renderer))
        throw failure('VIDEO_RENDER_UNSUPPORTED', 'Invalid native video surface descriptor');
      if (!active) { closing = release(); await closing; throw status; }
      if (closed || globalThis.document?.hidden) await stop();
      return surface;
    } catch (error) {
      await stop(error).catch(() => {});
      throw error;
    }
  };
  const openWorkerVideo = async (worker, options = {}) => {
    if (typeof Worker !== 'function' || !(worker instanceof Worker) || videoWorkers.has(worker))
      throw failure('INVALID_ARGUMENT', 'Expected an idle dedicated video Worker');
    if (!nativeBuffers) throw failure('UNSUPPORTED_FORMAT', 'Worker video delivery requires WebView2');
    if (globalThis.document?.hidden) throw failure('VIDEO_PAGE_HIDDEN', 'Show the page before opening project video');
    videoWorkers.add(worker);
    let descriptor, buffer, port, active = true, transferred = false, finished = false, closing, status = null, timeout;
    let readyResolve, readyReject, closeResolve;
    const ready = new Promise((resolve, reject) => { readyResolve = resolve; readyReject = reject; });
    // Opening can fail before a worker receives its port.
    ready.catch(() => {});
    const listeners = new Map();
    const announce = (name, value) => { for (const listener of listeners.get(name) || []) notify(listener, value); };
    const release = returned => {
      if (returned?.byteLength) {
        try { nativeBuffers.releaseBuffer(returned); }
        catch { worker.terminate(); }
      }
    };
    const finish = async returned => {
      if (finished) return;
      finished = true;
      clearTimeout(timeout); release(returned); release(buffer); buffer = null;
      port?.close(); worker.removeEventListener('error', crashed);
      streamConsumers.delete(consumer); videoConsumers.delete(consumer); videoWorkers.delete(worker);
      try { if (descriptor && !closed) await call('ReaWeb_StreamDetach', [descriptor.token]); }
      finally { closeResolve?.(); }
    };
    const stop = (code = 'STREAM_CLOSED', message = 'Consumer detached', terminate = false) => {
      if (closing) return closing;
      if (finished) return Promise.resolve();
      active = false; status = failure(code, message);
      readyReject(status);
      closing = new Promise(resolve => { closeResolve = resolve; });
      if (code !== 'STREAM_CLOSED') announce('error', status);
      announce('close', status); listeners.clear();
      if (transferred && !terminate) {
        timeout = setTimeout(() => { worker.terminate(); void finish().catch(() => {}); }, 1000);
        try { port.postMessage({ type: 'close' }); }
        catch { clearTimeout(timeout); worker.terminate(); void finish().catch(() => {}); }
      } else {
        if (terminate) worker.terminate();
        void finish().catch(() => {});
      }
      return closing;
    };
    const crashed = event => { void stop('TRANSPORT_ERROR', event.message || 'Video worker failed', true); };
    const consumer = Object.freeze({
      get info() { return Object.freeze(Object.fromEntries(Object.entries(descriptor || {}).filter(([key]) => !['token', 'url', 'sharedVideo', 'videoResource', 'videoNativeCopy'].includes(key)))); },
      get closed() { return !active; }, get error() { return status; },
      on: (event, callback) => {
        if (!['data', 'close', 'error'].includes(event) || typeof callback !== 'function') throw failure('INVALID_ARGUMENT', 'Expected data, close or error and a callback');
        if (!active) { if (event === 'close') notify(callback, status); return () => {}; }
        if (!listeners.has(event)) listeners.set(event, new Set());
        listeners.get(event).add(callback); return () => listeners.get(event)?.delete(callback);
      },
      close: () => stop()
    });
    streamConsumers.add(consumer); videoConsumers.add(consumer);
    worker.addEventListener('error', crashed);
    try {
      const name = await call('ReaWeb_VideoOpen', [options]);
      if (!active) throw status;
      descriptor = await call('ReaWeb_StreamOpen', [name]);
      if (!active) { if (!closed) await call('ReaWeb_StreamDetach', [descriptor.token]); throw status; }
      if (descriptor.kind !== 'frame' || !['rgba8', 'i420'].includes(descriptor.format) || descriptor.videoResource || descriptor.videoNativeCopy)
        throw failure('UNSUPPORTED_FORMAT', 'Expected project video memory or a binary video stream');
      if (descriptor.sharedVideo) {
        if (!nativeBuffers.releaseBuffer) throw failure('UNSUPPORTED_FORMAT', 'Native video mapping is unavailable');
        buffer = await takeVideoBuffer(descriptor.token);
        if (!active) { release(buffer); buffer = null; throw status; }
        if (buffer.byteLength !== descriptor.maxBytes * 2) throw failure('UNSUPPORTED_FORMAT', 'Invalid shared video buffer');
      }
      const channel = new MessageChannel(); port = channel.port1;
      port.onmessage = ({ data }) => {
        if (!data || typeof data !== 'object') { void stop('UNSUPPORTED_FORMAT', 'Invalid video worker message'); return; }
        if (data.type === 'closed') {
          if (active) {
            active = false; status = failure(data.code || 'STREAM_CLOSED', data.message || 'Video worker closed'); readyReject(status);
            closing = new Promise(resolve => { closeResolve = resolve; });
            if (status.code !== 'STREAM_CLOSED') announce('error', status);
            announce('close', status); listeners.clear();
          }
          void finish(data.buffer).catch(() => {});
        } else if (active && data.type === 'ready') { clearTimeout(timeout); readyResolve(consumer); }
        else if (active && data.type === 'error') void stop(data.code || 'TRANSPORT_ERROR', data.message || 'Video worker failed');
        else if (active && data.type === 'frame') {
          const frame = data.frame;
          if (typeof frame?.sequence !== 'bigint' || frame.sequence < 0n || typeof frame.producerDropped !== 'bigint' || frame.producerDropped < 0n ||
              !Number.isFinite(frame.timestamp)) { void stop('UNSUPPORTED_FORMAT', 'Invalid video frame metadata'); return; }
          if (descriptor.dynamicResolution && (!Number.isInteger(frame.width) || !Number.isInteger(frame.height) ||
              frame.width <= 0 || frame.height <= 0 || frame.width > 8192 || frame.height > 8192 ||
              frame.width * frame.height * 4 > 40 * 1024 * 1024 ||
              frame.stride !== frame.width * (descriptor.format === 'i420' ? 1 : 4) ||
              (descriptor.format === 'i420' && (frame.width % 2 || frame.height % 2)) ||
              frame.stride * frame.height * (descriptor.format === 'i420' ? 1.5 : 1) > descriptor.maxBytes)) {
            void stop('UNSUPPORTED_FORMAT', 'Invalid video frame dimensions'); return;
          }
          announce('data', Object.freeze({ sequence: frame.sequence, frameId: frame.sequence, timestamp: frame.timestamp,
            producerDropped: frame.producerDropped, discontinuity: !!frame.discontinuity,
            width: frame.width ?? descriptor.width, height: frame.height ?? descriptor.height, stride: frame.stride ?? descriptor.stride }));
        }
      };
      port.onmessageerror = () => { void stop('TRANSPORT_ERROR', 'Video worker message could not be read'); };
      worker.postMessage({ type: 'reaweb-video', port: channel.port2, descriptor, buffer }, buffer ? [channel.port2, buffer] : [channel.port2]);
      transferred = true; buffer = null;
      timeout = setTimeout(() => { void stop('TIMEOUT', 'Video worker connection timed out'); }, 10000);
      if (globalThis.document?.hidden || closed) void stop();
      return await ready;
    } catch (error) {
      await stop(error.code || 'TRANSPORT_ERROR', error.message);
      throw error;
    }
  };
  globalThis.document?.addEventListener?.('visibilitychange', () => {
    if (document.hidden) for (const consumer of videoConsumers) consumer.close().catch(() => {});
  });
  api.video = Object.freeze({
    getCapabilities: () => call('ReaWeb_VideoCapabilities', []),
    getSourceInfo: () => call('ReaWeb_VideoSourceInfo', []),
    getState: () => call('ReaWeb_VideoState', []),
    getDiagnostics: () => call('ReaWeb_VideoDiagnostics', []),
    openSurface: openVideoSurface,
    openWorkerStream: openWorkerVideo,
    openStream: async (options = {}) => {
      if (globalThis.document?.hidden) throw failure('VIDEO_PAGE_HIDDEN', 'Show the page before opening project video');
      const consumer = await openStream(await call('ReaWeb_VideoOpen', [options]));
      videoConsumers.add(consumer);
      consumer.on('close', () => videoConsumers.delete(consumer));
      if (globalThis.document?.hidden) await consumer.close();
      return consumer;
    }
  });
  // Realtime sources use ordinary v1 binary streams. JSFX v2 timing and its
  // instance permissions remain separate; source decoders are explicitly versioned.
  const realtimeDecoders = {
    'transport.v1': packet => {
      const bytes = packet.bytes;
      if (bytes.byteLength !== 64) throw failure('UNSUPPORTED_FORMAT', 'Invalid transport snapshot size');
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      if (view.getUint32(0, true) !== 0x31305452) throw failure('UNSUPPORTED_FORMAT', 'Invalid transport snapshot version');
      const sampledAt = view.getFloat64(8, true), position = view.getFloat64(16, true);
      const playState = view.getUint32(24, true), sampleDurationMs = view.getFloat64(40, true) * 1000;
      if (!Number.isFinite(sampledAt) || !Number.isFinite(position) || !Number.isFinite(sampleDurationMs) || sampleDurationMs < 0 || playState > 7)
        throw failure('UNSUPPORTED_FORMAT', 'Invalid transport snapshot values');
      return Object.freeze({ source: 'transport', position, playState, sampledAt, sampleDurationMs,
        projectChanged: !!(view.getUint32(28, true) & 1), projectEpoch: view.getBigUint64(32, true),
        sourceSequence: view.getBigUint64(48, true), sequence: packet.sequence,
        producerDropped: packet.producerDropped });
    },
    'transport.v2': packet => {
      const bytes = packet.bytes;
      if (bytes.byteLength !== 96) throw failure('UNSUPPORTED_FORMAT', 'Invalid transport snapshot size');
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      if (view.getUint32(0, true) !== 0x32305452) throw failure('UNSUPPORTED_FORMAT', 'Invalid transport snapshot version');
      const sampledAt = view.getFloat64(8, true), position = view.getFloat64(16, true);
      const playState = view.getUint32(24, true), sampleDurationMs = view.getFloat64(40, true) * 1000;
      if (!Number.isFinite(sampledAt) || !Number.isFinite(position) || !Number.isFinite(sampleDurationMs) || sampleDurationMs < 0 || playState > 7)
        throw failure('UNSUPPORTED_FORMAT', 'Invalid transport snapshot values');
      const playRate = view.getFloat64(64, true), tempo = view.getFloat64(72, true), beatPosition = view.getFloat64(80, true);
      const numerator = view.getUint32(88, true), denominator = view.getUint32(92, true);
      if (!Number.isFinite(playRate) || playRate <= 0 || !Number.isFinite(tempo) || tempo <= 0 ||
          !Number.isFinite(beatPosition) || !numerator || !denominator)
        throw failure('UNSUPPORTED_FORMAT', 'Invalid transport musical values');
      return Object.freeze({ source: 'transport', position, playState, playRate, tempo, beatPosition,
        timeSignature: Object.freeze({ numerator, denominator }), sampledAt, sampleDurationMs,
        projectChanged: !!(view.getUint32(28, true) & 1), projectEpoch: view.getBigUint64(32, true),
        sourceSequence: view.getBigUint64(48, true), sequence: packet.sequence,
        producerDropped: packet.producerDropped });
    }
  };
  api.realtime = Object.freeze({
    getCapabilities: () => call('ReaWeb_RealtimeCapabilities', []),
    getDiagnostics: () => call('ReaWeb_RealtimeDiagnostics', []),
    subscribe: async (source, options = {}) => {
      if (typeof source !== 'string' || !/^[a-z][a-z0-9-]{0,63}$/.test(source) ||
          !options || typeof options !== 'object' || Array.isArray(options) ||
          Object.keys(options).some(key => !['updateRate', 'onData', 'onError', 'onClose'].includes(key)) ||
          ['onData', 'onError', 'onClose'].some(key => options[key] !== undefined && typeof options[key] !== 'function') ||
          (options.updateRate !== undefined && (!Number.isFinite(options.updateRate) || options.updateRate < 1 || options.updateRate > 120)))
        throw failure('INVALID_ARGUMENT', 'Expected source, updateRate (1..120) and optional callbacks');
      const descriptor = await call('ReaWeb_RealtimeSubscribe', [source, options.updateRate === undefined ? {} : { updateRate: options.updateRate }]);
      let stream, current = null, releasing = null, closing = null, error = null;
      const release = () => {
        if (!releasing) releasing = closed ? Promise.resolve() : call('ReaWeb_RealtimeUnsubscribe', [descriptor.id]);
        return releasing;
      };
      try {
        const decode = realtimeDecoders[descriptor.schema];
        if (!decode) throw failure('UNSUPPORTED_FORMAT', 'Unknown realtime source schema');
        stream = await openStream(descriptor.stream);
        const receive = () => {
          const packet = stream.latest();
          if (!packet || current?.sequence === packet.sequence) return;
          try { current = decode(packet); if (options.onData) notify(options.onData, current); }
          catch (reason) { error = reason; if (options.onError) notify(options.onError, reason); void subscription.close().catch(() => {}); }
        };
        const subscription = Object.freeze({
          source, updateRate: descriptor.updateRate,
          latest: () => stream.closed ? null : current,
          get closed() { return stream.closed; },
          get error() { return error || stream.error; },
          close: () => {
            if (!closing) closing = (async () => { try { await stream.close(); } finally { await release(); } })();
            return closing;
          }
        });
        stream.on('data', receive);
        stream.on('error', reason => { error = reason; if (options.onError) notify(options.onError, reason); });
        stream.on('close', reason => {
          current = null;
          void release().catch(reason => { if (options.onError) notify(options.onError, reason); });
          if (options.onClose) notify(options.onClose, reason);
        });
        receive();
        return subscription;
      } catch (reason) {
        try { if (stream) await stream.close(); } finally { await release().catch(() => {}); }
        throw reason;
      }
    }
  });
  const nativeTask = async (eventName, start, args, end, callback, waitReady) => {
    if (typeof callback !== 'function') throw failure('INVALID_ARGUMENT', 'Expected a task callback');
    let id, stopped = false, initializedReady = false, resolveReady, rejectReady;
    const early = [];
    const initialized = new Promise((resolve, reject) => { resolveReady = resolve; rejectReady = reject; });
    initialized.catch(() => {});
    const accept = event => {
      if (id === undefined) { if (early.length < 256) early.push(event); return; }
      if (event.id !== id || stopped) return;
      if (event.type === 'ready') { initializedReady = true; resolveReady(); }
      else if (event.type === 'error' && waitReady && !initializedReady) rejectReady(failure(event.code, event.message));
      else {
        notify(callback, event);
        if (!waitReady && !args[0].interval) void dispose();
      }
    };
    const off = await onEvent(eventName, accept);
    const dispose = async () => { if (stopped) return; stopped = true; await off(); if (!closed && id !== undefined) await call(end, [id]); };
    try {
      id = await call(start, args); for (const event of early) accept(event);
      if (waitReady) {
        const timeout = setTimeout(() => rejectReady(failure('TIMEOUT', 'File watch initialization timed out')), 10000);
        try { await initialized; } finally { clearTimeout(timeout); }
      }
      return dispose;
    } catch (error) { await dispose(); throw error; }
  };
  // Runtime namespaces share the existing bridge; the 730 REAPER methods retain
  // their names, argument order, typed handles and asynchronous results.
  api.events = Object.freeze({ on: onEvent, off: offEvent });
  api.host = Object.freeze({ service, send: async message => {
    let text;
    try {
      text = typeof message === 'string' ? message : JSON.stringify(message, (_, value) => {
        if (['undefined', 'function', 'symbol', 'bigint'].includes(typeof value) ||
            (typeof value === 'number' && !Number.isFinite(value)))
          throw new TypeError('Expected a JSON-serializable value');
        return value;
      });
    } catch (error) { throw failure('INVALID_ARGUMENT', error.message); }
    if (typeof text !== 'string' || text.includes('\0'))
      throw failure('INVALID_ARGUMENT', 'Host messages must be text without NUL or a JSON-serializable value');
    if (new TextEncoder().encode(text).length > 1024 * 1024)
      throw failure('MESSAGE_LIMIT', 'Host message exceeds 1 MiB of UTF-8 text');
    return call('ReaWeb_HostSend', [text]);
  } });
  api.window = Object.freeze({
    open: (path, options) => options === undefined ? call('ReaWeb_Open', [path]) : call('ReaWeb_OpenEx', [path, options]), openDev: host('ReaWeb_OpenDev'),
    getSize: async () => { const b = await call('ReaWeb_GetBounds', []); return { width: b.width, height: b.height, mode: b.mode, units: b.units }; },
    setSize: (width, height) => call('ReaWeb_SetBounds', [{ width, height }]),
    getPosition: async () => { const b = await call('ReaWeb_GetBounds', []); return { x: b.x, y: b.y, mode: b.mode, units: b.units }; },
    setPosition: (x, y) => call('ReaWeb_SetBounds', [{ x, y }]),
    show: () => call('ReaWeb_SetVisible', [true]), hide: () => call('ReaWeb_SetVisible', [false]),
    getState: host('ReaWeb_GetWindowState'), setTitle: host('ReaWeb_SetTitle'), setIcon: host('ReaWeb_SetIcon'),
    setIconVisible: host('ReaWeb_SetIconVisible'), focus: host('ReaWeb_Focus'),
    setFrameless: host('ReaWeb_SetFrameless'), setCornerRadius: host('ReaWeb_SetCornerRadius'),
    setDocked: host('ReaWeb_SetDocked'), isDocked: host('ReaWeb_IsDocked'),
    setMouseMode: host('ReaWeb_SetMouseMode'),
    setMouseRegions: host('ReaWeb_SetMouseRegions'),
    setKeyboardCapture: host('ReaWeb_SetKeyboardCapture'),
    setShortcutRouting: host('ReaWeb_SetShortcutRouting'),
    setDawKeyboard: host('ReaWeb_SetDawKeyboard'),
    close: host('ReaWeb_Close'), reload: host('ReaWeb_Reload')
  });
  api.fs = Object.freeze({
    openMedia: async path => {
      const descriptor = await call('ReaWeb_OpenMedia', [path]);
      let closing;
      return Object.freeze({ ...descriptor, close: () => closing || (closing = call('ReaWeb_CloseMedia', [descriptor.url]).catch(error => { closing = null; throw error; })) });
    },
    watch: (path, callback, options = {}) => nativeTask('file-change', 'ReaWeb_WatchBegin', [path, options], 'ReaWeb_WatchEnd', callback, true),
    readFile: host('ReaWeb_ReadFile'), writeFile: host('ReaWeb_WriteFile'),
    readText: path => call('ReaWeb_ReadFile', [path, { encoding: 'utf8' }]),
    writeText: (path, text, options = {}) => call('ReaWeb_WriteFile', [path, text, { ...options, encoding: 'utf8' }]),
    readBinary: path => call('ReaWeb_ReadFile', [path, { encoding: 'binary' }]),
    writeBinary: (path, bytes, options = {}) => call('ReaWeb_WriteFile', [path, bytes, { ...options, encoding: 'binary' }]),
    stat: host('ReaWeb_Stat'), readDirectory: host('ReaWeb_ReadDirectory'), makeDirectory: host('ReaWeb_MakeDirectory'),
    moveFile: host('ReaWeb_MoveFile')
  });
  api.clipboard = Object.freeze({
    readText: host('ReaWeb_ClipboardReadText'), writeText: host('ReaWeb_ClipboardWriteText'),
    readBinary: host('ReaWeb_ClipboardReadBinary'), writeBinary: host('ReaWeb_ClipboardWriteBinary')
  });
  api.system = Object.freeze({
    getDevices: host('ReaWeb_GetDevices'),
    getDisplays: host('ReaWeb_GetDisplays'),
    openMIDIInput: async (device = -1) => openStream(await call('ReaWeb_MIDIOpen', [device])),
    schedule: (callback, options = {}) => nativeTask('native-timer', 'ReaWeb_TimerStart', [options], 'ReaWeb_TimerStop', callback, false),
    getCapabilities: async () => { await ready; await probeBrowser(); return call('ReaWeb_GetCapabilities', []); }, openExternal: host('ReaWeb_OpenExternal'),
    getPlatform: host('ReaWeb_GetPlatform'), getArchitecture: host('ReaWeb_GetArchitecture'), revealInFileManager: host('ReaWeb_RevealPath')
  });
  api.transaction = Object.freeze({
    batch, beginUndo: host('ReaWeb_BeginUndo'), endUndo: host('ReaWeb_EndUndo'), withUndo
  });
  api.dragDrop = Object.freeze({
    startFiles: host('ReaWeb_DragFiles'), startText: host('ReaWeb_DragText')
  });
  const appValue = key => async () => (await call('ReaWeb_GetAppInfo', []))[key];
  api.app = Object.freeze({ getId: appValue('id'), getName: appValue('name'), getVersion: appValue('version'),
    getRootPath: appValue('rootPath'), getDataPath: appValue('dataPath') });
  // WebView2 converts real dropped File objects into native ICoreWebView2File
  // objects. Never infer native filesystem paths from browser file names.
  if (window.chrome?.webview) {
    const dropActive = () => !!subscriptions.get('native-drop')?.listeners.size;
    window.addEventListener('dragover', event => {
      if (dropActive() && event.isTrusted) { event.preventDefault(); if (event.dataTransfer) event.dataTransfer.dropEffect = 'copy'; }
    });
    window.addEventListener('drop', event => {
      if (!dropActive() || !event.isTrusted || !event.dataTransfer) return;
      event.preventDefault();
      try {
        const files = [...event.dataTransfer.files];
        const text = event.dataTransfer.getData('text/plain');
        if (files.length > 256 || new TextEncoder().encode(text).length > 16 * 1024 * 1024) throw failure('BUFFER_LIMIT', 'Native drop exceeds the payload limit');
        const message = JSON.stringify({__reawebNativeDrop: {
          document: documentId, text, x: event.clientX, y: event.clientY
        }});
        if (new TextEncoder().encode(message).length > 64 * 1024 * 1024) throw failure('MESSAGE_LIMIT', 'Native drop exceeds the transport limit');
        window.chrome.webview.postMessageWithAdditionalObjects(message, files);
      } catch (error) { console.error('[ReaWebAPI drop]', error); }
    });
  }
  const dialog = async (mode, options = {}) => {
    if (!options || typeof options !== 'object' || Array.isArray(options)) throw failure('INVALID_ARGUMENT', 'Expected dialog options');
    const { title = '', initialPath = '', filters = [] } = options;
    if (typeof title !== 'string' || typeof initialPath !== 'string' || !Array.isArray(filters))
      throw failure('INVALID_ARGUMENT', 'Invalid dialog options');
    const extension = filters.map(filter => {
      if (!filter || typeof filter.name !== 'string' || /[|\0]/.test(filter.name) || !Array.isArray(filter.extensions) || !filter.extensions.length ||
          filter.extensions.some(value => typeof value !== 'string' || !/^(\*|[a-zA-Z0-9][a-zA-Z0-9._-]*)$/.test(value)))
        throw failure('INVALID_ARGUMENT', 'Filters need a name and extensions such as wav, aiff or *');
      return filter.name + '|' + filter.extensions.map(value => value === '*' ? '*.*' : '*.' + value).join(';');
    }).join('|');
    const [ok, path] = await api.GetUserFileName(mode, title, initialPath, extension);
    return ok ? path : null;
  };
  api.dialog = Object.freeze({ openFile: options => dialog(1, options), saveFile: options => dialog(0, options), selectFolder: options => dialog(3, options) });
  const preview = values => {
    const seen = new WeakSet();
    try { return values.map(value => typeof value === 'string' ? value : JSON.stringify(value, (_, item) => {
      if (typeof item === 'bigint') return String(item);
      if (item instanceof Error) return { name: item.name, message: item.message, stack: item.stack };
      if (item && typeof item === 'object') { if (seen.has(item)) return '[Circular]'; seen.add(item); }
      return item;
    })).join(' ').slice(0, 4000); } catch { return '[Unserializable log value]'; }
  };
  const log = (level, values) => call('ReaWeb_Log', [{ level, message: preview(values) }]);
  api.debug = Object.freeze({
    log: (...values) => log('info', values),
    warn: (...values) => log('warn', values), error: (...values) => log('error', values),
    inspect: value => log('debug', [value]), getLogs: () => call('ReaWeb_GetLogs', []),
    getDiagnostics: async () => { await ready; await probeBrowser(); return call('ReaWeb_GetDiagnostics', []); }, openDevTools: host('ReaWeb_DevTools'),
    setBufferSize: host('ReaWeb_SetBufferSize')
  });
  window.addEventListener('error', event => {
    if (diagnosticEnabled) {
      ++health.scriptErrors;
      health.scriptLine = Number.isSafeInteger(event.lineno) && event.lineno > 0 ? event.lineno : 0;
      health.scriptColumn = Number.isSafeInteger(event.colno) && event.colno > 0 ? event.colno : 0;
    }
    log('error', [event.error || event.message || 'JavaScript error']).catch(() => {});
  });
  window.addEventListener('unhandledrejection', event => { if (diagnosticEnabled) ++health.promiseErrors; log('error', [event.reason || 'Unhandled promise rejection']).catch(() => {}); });
  api.theme = Object.freeze({
    getColors: () => call('ReaWeb_GetTheme', []),
    apply: async (element = window.document?.documentElement) => {
      if (!element?.style?.setProperty) throw failure('INVALID_ARGUMENT', 'Expected an element with a CSS style');
      const previous = new Map();
      const stop = await onEvent('theme-changed', theme => {
        for (const [name, value] of Object.entries(theme.cssVariables)) {
          if (!previous.has(name)) previous.set(name, [element.style.getPropertyValue(name), element.style.getPropertyPriority(name)]);
          element.style.setProperty(name, value);
        }
      });
      return async () => { await stop(); for (const [name, [value, priority]] of previous) {
        if (value) element.style.setProperty(name, value, priority); else element.style.removeProperty(name);
      } };
    }
  });
  api.audio = Object.freeze({
    openStream: async (kind, options = {}) => openStream(await call('ReaWeb_AnalysisOpen', [kind, options])),
    resetMeter: name => call('ReaWeb_MeterReset', [name]),
    decodeMeter: (data, channels) => {
      if (!(data instanceof Float32Array) || !Number.isInteger(channels) || channels < 1 || channels > 32 || data.length !== 13 * channels + 14)
        throw new TypeError('Expected a built-in meter payload and its channel count');
      const values = {}, base = 7 * channels, tail = base + 14;
      ['samplePeak', 'truePeak', 'channelRms'].forEach((name, group) => { values[name] = data.slice(group * channels, (group + 1) * channels); });
      ['channelMaxSamplePeak', 'channelMaxTruePeak'].forEach((name, group) => { values[name] = data.slice((5 + group) * channels, (6 + group) * channels); });
      ['sampleClipCount', 'truePeakClipCount'].forEach((name, group) => {
        values[name] = Array.from({ length: channels }, (_, channel) => {
          const at = tail + group * 3 * channels + channel;
          const low = data[at], middle = data[at + channels], high = data[at + 2 * channels];
          if (![low, middle, high].every(Number.isInteger) || low < 0 || low > 0xffffff || middle < 0 || middle > 0xffffff || high < 0 || high > 0xffff)
            throw new TypeError('Invalid built-in meter counter');
          return BigInt(low) | (BigInt(middle) << 24n) | (BigInt(high) << 48n);
        });
      });
      ['rmsMomentary', 'rmsIntegrated', 'maxRmsMomentary', 'lufsMomentary', 'lufsShortTerm', 'lufsIntegrated', 'loudnessRange', 'maxLufsMomentary', 'maxLufsShortTerm', 'maxSamplePeak', 'maxTruePeak', 'processedSeconds', 'loudnessRangeLow', 'loudnessRangeHigh'].forEach((name, i) => { values[name] = data[base + i]; });
      return values;
    },
    getFileInfo: path => call('ReaWeb_AudioFileInfo', [path]),
    getWaveform: (path, options = {}) => call('ReaWeb_AudioWaveform', [path, options]),
    getTrackMeter: track => call('ReaWeb_GetTrackMeter', [track]),
    setTrackValueLatest
  });
  const runCleanup = async event => {
    if (!event || !['before-close', 'before-reload'].includes(event.event) || cleanupToken) return;
    cleanupToken = event.token;
    const tasks = [];
    for (const name of [event.event, 'cleanup']) for (const callback of lifecycleListeners.get(name) || []) {
      try { tasks.push(Promise.resolve(callback(Object.freeze({ reason: event.reason, timeoutMs: event.timeoutMs })))); }
      catch (error) { tasks.push(Promise.reject(error)); }
    }
    let timer;
    try {
      await Promise.race([Promise.allSettled(tasks), new Promise(resolve => { timer = setTimeout(resolve, Math.min(event.timeoutMs, 2000)); })]);
      await call('ReaWeb_LifecycleComplete', [event.token]);
    } catch (error) { if (!closed) console.error('[ReaWebAPI cleanup]', error); }
    finally { clearTimeout(timer); }
  };
  api.lifecycle = Object.freeze({
    ready,
    on: async (name, callback) => {
      if (!['before-close', 'before-reload', 'cleanup'].includes(name) || typeof callback !== 'function')
        throw failure('INVALID_ARGUMENT', 'Expected before-close, before-reload or cleanup and a callback');
      const wrapper = event => callback(event);
      if (!lifecycleListeners.has(name)) lifecycleListeners.set(name, new Set());
      const listeners = lifecycleListeners.get(name); listeners.add(wrapper);
      try { await call('ReaWeb_LifecycleSubscribe', [true]); }
      catch (error) { listeners.delete(wrapper); throw error; }
      let disposed = false;
      return async () => {
        if (disposed) return; disposed = true; listeners.delete(wrapper);
        if (![...lifecycleListeners.values()].some(list => list.size) && !closed) await call('ReaWeb_LifecycleSubscribe', [false]);
      };
    }
  });
  Object.defineProperty(window, 'reaper', { value: Object.freeze(api), enumerable: true });
  ready.then(startTitle).catch(() => {});
  ready.then(startFavicon).catch(() => {});
  window.addEventListener('pagehide', () => {
    stopTitle();
    stopFavicon();
    if (!cleanupToken) for (const callback of lifecycleListeners.get('cleanup') || [])
      notify(callback, Object.freeze({ reason: 'unload', timeoutMs: 0 }));
    closed = true;
    for (const consumer of [...streamConsumers]) void consumer.close();
    for (const item of pending.values()) {
      clearTimeout(item.timer);
      item.reject(failure('WINDOW_CLOSED', 'The WebView document was closed'));
    }
    pending.clear();
    subscriptions.clear();
    serviceSubscriptions.clear();
    lifecycleListeners.clear();
    for (const entry of latest.values()) {
      if (entry.next) entry.next.reject(failure('WINDOW_CLOSED', 'The WebView document was closed'));
      entry.next = null;
    }
    latest.clear();
  });
})();
