(() => {
  'use strict';
  if (window !== window.top || window.reaper) return;
  const pending = new Map();
  const subscriptions = new Map();
  const latest = new Map();
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
    try { callback(data); } catch (error) { console.error('[ReaWebAPI event]', error); }
  };
  const receive = message => {
    if (closed || message?.document !== documentId) return;
    if (message.event) {
      if (!Number.isSafeInteger(message.sequence) || message.sequence <= eventSequence) return;
      eventSequence = message.sequence;
      if (message.event === 'projectchange') projectEpoch = message.data.projectEpoch;
      const entry = subscriptions.get(message.event);
      if (entry) {
        entry.last = message.data;
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
    } else item.resolve(decodeValue(message.result));
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
    pending.set(id, { resolve, reject, timer });
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
  const api = Object.create(null);
  const methods = [
    ...reawebApiMethods, 'ReaWebOpen',
    'ReaWeb_Close', 'ReaWeb_DevTools', 'ReaWeb_SetDocked', 'ReaWeb_IsDocked', 'ReaWeb_GetCapabilities',
    'ReaWeb_Batch', 'ReaWeb_GetWindowState', 'ReaWeb_GetDiagnostics', 'ReaWeb_Focus',
    'ReaWeb_SetTitle', 'ReaWeb_SetKeyboardCapture', 'ReaWeb_SetBufferSize',
    'ReaWeb_OpenDev', 'ReaWeb_BeginUndo', 'ReaWeb_EndUndo',
    'ReaWeb_ReadFile', 'ReaWeb_WriteFile', 'ReaWeb_Stat', 'ReaWeb_ReadDirectory', 'ReaWeb_MakeDirectory',
    'ReaWeb_ClipboardReadText', 'ReaWeb_ClipboardWriteText', 'ReaWeb_OpenExternal'
  ];
  api.ready = ready;
  for (const name of methods) api[name] = (...args) => call(name, args).then(result => {
    if (reawebApiMethods.includes(name) && result?.__reawebCall === true) {
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
  api.ReaWeb_On = async (name, callback) => {
    if (!['projectchange', 'selectionchange', 'itemselectionchange', 'takeselectionchange', 'transportchange', 'fxchange', 'windowstatechange'].includes(name))
      throw failure('UNKNOWN_EVENT', 'Unknown host event');
    if (typeof callback !== 'function') throw failure('INVALID_ARGUMENT', 'Expected an event callback');
    let entry = subscriptions.get(name);
    if (!entry) {
      entry = { listeners: new Set(), initial: call('ReaWeb_Subscribe', [name]), last: null };
      subscriptions.set(name, entry);
    }
    // A wrapper allows the same function to have independent subscriptions.
    const listener = data => callback(data);
    entry.listeners.add(listener);
    try {
      const initial = await entry.initial;
      if (closed) throw failure('WINDOW_CLOSED', 'The WebView document was closed');
      if (entry.last ?? initial) notify(listener, entry.last ?? initial);
    } catch (error) {
      entry.listeners.delete(listener);
      if (!entry.listeners.size && subscriptions.get(name) === entry) subscriptions.delete(name);
      throw error;
    }
    let disposed = false;
    return async () => {
      if (disposed) return;
      disposed = true;
      entry.listeners.delete(listener);
      if (!entry.listeners.size && subscriptions.get(name) === entry) {
        subscriptions.delete(name);
        if (!closed) await call('ReaWeb_Unsubscribe', [name]);
      }
    };
  };
  api.ReaWeb_WithUndo = async (label, callback) => {
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
  api.ReaWeb_SetTrackValueLatest = (track, key, value) => new Promise((resolve, reject) => {
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
  Object.defineProperty(window, 'reaper', { value: Object.freeze(api), enumerable: true });
  window.addEventListener('pagehide', () => {
    closed = true;
    for (const item of pending.values()) {
      clearTimeout(item.timer);
      item.reject(failure('WINDOW_CLOSED', 'The WebView document was closed'));
    }
    pending.clear();
    subscriptions.clear();
    for (const entry of latest.values()) {
      if (entry.next) entry.next.reject(failure('WINDOW_CLOSED', 'The WebView document was closed'));
      entry.next = null;
    }
    latest.clear();
  });
})();
