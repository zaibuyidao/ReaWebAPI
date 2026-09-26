(() => {
  'use strict';
  if (window !== window.top || window.reaper) return;
  const pending = new Map();
  const subscriptions = new Map();
  const serviceSubscriptions = new Map();
  const streamConsumers = new Set();
  const latest = new Map();
  const batchReferences = new WeakMap();
  const eventNames = ['projectchange', 'selectionchange', 'itemselectionchange', 'takeselectionchange',
    'transportchange', 'fxchange', 'windowstatechange', 'track-added', 'track-deleted', 'track-selected',
    'item-changed', 'take-changed', 'playback-state-changed', 'tempo-changed', 'marker-changed',
    'fx-changed', 'project-loaded', 'project-saved', 'theme-changed', 'native-drop', 'message',
    'trackSelectionChanged', 'trackStateChanged', 'transportChanged', 'projectChanged', 'markersChanged',
    'regionsChanged', 'currentRegionChanged', 'loopPointsChanged', 'timeSelectionChanged', 'file-change', 'native-timer'];
  const discreteEvents = new Set(['native-drop', 'message', 'file-change', 'native-timer']);
  const lifecycleListeners = new Map();
  let cleanupToken = null;
  const documentId = Array.from(crypto.getRandomValues(new Uint32Array(4)), n => n.toString(16)).join('-');
  let sequence = 0, eventSequence = 0, projectEpoch = 0, waiting = 0, closed = false;
  const post = window.chrome?.webview
    ? message => window.chrome.webview.postMessage(message)
    : window.webkit?.messageHandlers?.reaweb
      ? message => window.webkit.messageHandlers.reaweb.postMessage(message)
      : null;
  const failure = (code, message, details) => Object.assign(new Error(message), { code, ...(details ? { details } : {}) });
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
    try { Promise.resolve(callback(data)).catch(error => console.error('[ReaWebAPI event]', error)); }
    catch (error) { console.error('[ReaWebAPI event]', error); }
  };
  const receive = message => {
    if (closed || message?.document !== documentId) return;
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
      if (!Number.isSafeInteger(message.sequence) || message.sequence <= eventSequence) return;
      eventSequence = message.sequence;
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
    if (message.started === true) { clearTimeout(item.timer); return; }
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
    if (pending.size >= 256) return reject(failure('QUEUE_LIMIT', 'Too many pending API calls'));
    const id = ++sequence;
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(failure('TIMEOUT', `${method} timed out; check the tool state before repeating a write`));
    }, 30000);
    pending.set(id, { resolve, reject, timer, service: method === 'ReaWeb_ServiceInvoke' });
    try {
      const message = JSON.stringify({ id, document: documentId, project: projectEpoch, expiresAt: Date.now() + 25000, method, args }, encodeValue);
      if (new TextEncoder().encode(message).length > 64 * 1024 * 1024) throw failure('MESSAGE_LIMIT', 'Bridge message exceeds 64 MiB');
      post(message);
    } catch (error) {
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
  const call = (method, args) => {
    if (closed) return Promise.reject(failure('WINDOW_CLOSED', 'The WebView document was closed'));
    if (waiting >= 256) return Promise.reject(failure('QUEUE_LIMIT', 'Too many pending API calls'));
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
          if (format && ['http:', 'https:', 'data:', 'blob:'].includes(url.protocol)) icon = { url: url.href, format };
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
    if (!eventNames.includes(name))
      throw failure('UNKNOWN_EVENT', 'Unknown host event');
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
    if (!eventNames.includes(name)) throw failure('UNKNOWN_EVENT', 'Unknown host event');
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
  const openStream = async name => {
    if (typeof name !== 'string' || !/^[A-Za-z0-9_.-]{1,128}$/.test(name)) throw failure('INVALID_ARGUMENT', 'Expected a native stream name');
    const descriptor = await call('ReaWeb_StreamOpen', [name]);
    let socket, active = true, last = null, status = null, consumerDrops = 0;
    const listeners = new Map(), queue = [];
    const ordered = ['audio', 'midi'].includes(descriptor.kind);
    const announce = (event, data) => { for (const listener of listeners.get(event) || []) notify(listener, data); };
    const finish = (code, message) => {
      if (!active) return;
      active = false; status = failure(code, message || code); queue.length = 0; last = null;
      streamConsumers.delete(consumer); announce('close', status); listeners.clear();
    };
    const consumer = Object.freeze({
      info: Object.freeze(Object.fromEntries(Object.entries(descriptor).filter(([key]) => key !== 'token' && key !== 'url'))),
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
      await new Promise((resolve, reject) => {
        socket = new WebSocket(descriptor.url); socket.binaryType = 'arraybuffer';
        const timeout = setTimeout(() => { socket.close(); reject(failure('TIMEOUT', 'Stream connection timed out')); }, 10000);
        let connected = false;
        socket.onopen = () => { connected = true; clearTimeout(timeout); resolve(); };
        socket.onerror = () => {
          const error = failure('TRANSPORT_ERROR', 'Native stream connection failed'); announce('error', error);
          if (!connected) { clearTimeout(timeout); reject(error); }
        };
        socket.onclose = event => {
          clearTimeout(timeout);
          const code = ['STREAM_CLOSED', 'EXTENSION_UNLOADED', 'TIMEOUT', 'UNSUPPORTED_FORMAT', 'NATIVE_ERROR'].includes(event.reason) ? event.reason : 'TRANSPORT_ERROR';
          finish(code, event.reason || 'Native stream connection closed');
          if (!connected) reject(status || failure(code, code));
        };
        socket.onmessage = event => {
          if (!active) return;
          try {
            const buffer = event.data, view = new DataView(buffer);
            if (buffer.byteLength < 40 || view.getUint32(0, true) !== 0x01535752 ||
                view.getUint8(4) !== ['', 'frame', 'audio', 'spectrum', 'meter', 'waveform', 'binary', 'midi'].indexOf(descriptor.kind) ||
                Number(view.getBigUint64(32, true)) !== buffer.byteLength - 40 || buffer.byteLength - 40 > descriptor.maxBytes)
              throw failure('UNSUPPORTED_FORMAT', 'Invalid stream packet');
            const sequence = view.getBigUint64(8, true), timestamp = view.getFloat64(16, true), dropped = view.getBigUint64(24, true);
            const bytes = new Uint8Array(buffer, 40);
            const data = descriptor.format === 'float32' ? new Float32Array(buffer, 40) : bytes;
            const packet = Object.freeze({ sequence, frameId: sequence, timestamp, producerDropped: dropped, data, bytes });
            last = packet;
            if (ordered) { if (queue.length < descriptor.capacity) queue.push(packet); else ++consumerDrops; }
            announce('data', packet);
            socket.send(new Uint8Array([1]));
          } catch (error) { announce('error', error); finish(error.code || 'UNSUPPORTED_FORMAT', error.message); socket.close(); }
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
    open: host('ReaWeb_Open'), openDev: host('ReaWeb_OpenDev'),
    getSize: async () => { const b = await call('ReaWeb_GetBounds', []); return { width: b.width, height: b.height, mode: b.mode, units: b.units }; },
    setSize: (width, height) => call('ReaWeb_SetBounds', [{ width, height }]),
    getPosition: async () => { const b = await call('ReaWeb_GetBounds', []); return { x: b.x, y: b.y, mode: b.mode, units: b.units }; },
    setPosition: (x, y) => call('ReaWeb_SetBounds', [{ x, y }]),
    show: () => call('ReaWeb_SetVisible', [true]), hide: () => call('ReaWeb_SetVisible', [false]),
    getState: host('ReaWeb_GetWindowState'), setTitle: host('ReaWeb_SetTitle'), setIcon: host('ReaWeb_SetIcon'),
    setIconVisible: host('ReaWeb_SetIconVisible'), focus: host('ReaWeb_Focus'),
    setDocked: host('ReaWeb_SetDocked'), isDocked: host('ReaWeb_IsDocked'),
    setKeyboardCapture: host('ReaWeb_SetKeyboardCapture'),
    close: host('ReaWeb_Close'), reload: host('ReaWeb_Reload')
  });
  api.fs = Object.freeze({
    watch: (path, callback, options = {}) => nativeTask('file-change', 'ReaWeb_WatchBegin', [path, options], 'ReaWeb_WatchEnd', callback, true),
    readFile: host('ReaWeb_ReadFile'), writeFile: host('ReaWeb_WriteFile'),
    readText: path => call('ReaWeb_ReadFile', [path, { encoding: 'utf8' }]),
    writeText: (path, text, options = {}) => call('ReaWeb_WriteFile', [path, text, { ...options, encoding: 'utf8' }]),
    readBinary: path => call('ReaWeb_ReadFile', [path, { encoding: 'binary' }]),
    writeBinary: (path, bytes, options = {}) => call('ReaWeb_WriteFile', [path, bytes, { ...options, encoding: 'binary' }]),
    stat: host('ReaWeb_Stat'), readDirectory: host('ReaWeb_ReadDirectory'), makeDirectory: host('ReaWeb_MakeDirectory')
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
    getCapabilities: host('ReaWeb_GetCapabilities'), openExternal: host('ReaWeb_OpenExternal'),
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
    getDiagnostics: host('ReaWeb_GetDiagnostics'), openDevTools: host('ReaWeb_DevTools'),
    setBufferSize: host('ReaWeb_SetBufferSize')
  });
  window.addEventListener('error', event => { log('error', [event.error || event.message || 'JavaScript error']).catch(() => {}); });
  window.addEventListener('unhandledrejection', event => { log('error', [event.reason || 'Unhandled promise rejection']).catch(() => {}); });
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
