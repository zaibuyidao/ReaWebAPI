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
    pending.delete(message.id);
    clearTimeout(item.timer);
    if (message.error) {
      if (message.error.code === 'PROJECT_CHANGED' && message.error.details?.projectEpoch)
        projectEpoch = message.error.details.projectEpoch;
      item.reject(failure(message.error.code, message.error.message, message.error.details));
    } else item.resolve(message.result);
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
      const message = JSON.stringify({ id, document: documentId, project: projectEpoch, expiresAt: Date.now() + 25000, method, args });
      if (new TextEncoder().encode(message).length > 65536) throw failure('MESSAGE_LIMIT', 'Bridge message exceeds 64 KiB');
      post(message);
    } catch (error) {
      clearTimeout(timer);
      pending.delete(id);
      reject(error);
    }
  });
  const ready = send('__reawebHello', [1]).then(capabilities => {
    if (capabilities?.protocol !== 1) throw failure('PROTOCOL_MISMATCH', 'Unsupported native bridge protocol');
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
    'CountTracks', 'CountSelectedTracks', 'GetTrack', 'GetSelectedTrack', 'GetTrackName',
    'GetMediaTrackInfo_Value', 'SetMediaTrackInfo_Value', 'GetAppVersion', 'ReaWebOpen',
    'ReaWeb_Close', 'ReaWeb_DevTools', 'ReaWeb_SetDocked', 'ReaWeb_IsDocked', 'ReaWeb_GetCapabilities',
    'ReaWeb_Batch', 'ReaWeb_GetWindowState', 'ReaWeb_GetDiagnostics', 'ReaWeb_Focus',
    'ReaWeb_SetTitle', 'ReaWeb_SetKeyboardCapture'
  ];
  api.ready = ready;
  for (const name of methods) api[name] = (...args) => call(name, args);
  api.ReaWeb_On = async (name, callback) => {
    if (!['projectchange', 'selectionchange', 'windowstatechange'].includes(name))
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
