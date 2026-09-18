(() => {
  'use strict';
  if (window !== window.top || window.reaper) return;
  const pending = new Map();
  const documentId = Array.from(crypto.getRandomValues(new Uint32Array(4)), n => n.toString(16)).join('-');
  let sequence = 0;
  const post = window.chrome?.webview
    ? message => window.chrome.webview.postMessage(message)
    : window.webkit?.messageHandlers?.reaweb
      ? message => window.webkit.messageHandlers.reaweb.postMessage(message)
      : null;
  const failure = (code, message) => Object.assign(new Error(message), { code });
  const receive = message => {
    if (message?.document !== documentId) return;
    const item = pending.get(message?.id);
    if (!item) return;
    pending.delete(message.id);
    clearTimeout(item.timer);
    if (message.error) item.reject(failure(message.error.code, message.error.message));
    else item.resolve(message.result);
  };
  Object.defineProperty(window, '__reawebReceive', { value: receive });
  const call = (method, args) => new Promise((resolve, reject) => {
    if (!post) return reject(failure('NO_RUNTIME', 'Open this page with ReaWebAPI inside REAPER'));
    if (pending.size >= 256) return reject(failure('QUEUE_LIMIT', 'Too many pending API calls'));
    const id = ++sequence;
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(failure('TIMEOUT', `${method} timed out; REAPER may be busy`));
    }, 30000);
    pending.set(id, { resolve, reject, timer });
    try {
      const message = JSON.stringify({ id, document: documentId, method, args });
      if (new TextEncoder().encode(message).length > 65536) throw failure('MESSAGE_LIMIT', 'Bridge message exceeds 64 KiB');
      post(message);
    } catch (error) {
      clearTimeout(timer);
      pending.delete(id);
      reject(error);
    }
  });
  const methods = [
    'CountTracks', 'CountSelectedTracks', 'GetTrack', 'GetSelectedTrack', 'GetTrackName',
    'GetMediaTrackInfo_Value', 'SetMediaTrackInfo_Value', 'GetAppVersion', 'ReaWebOpen',
    'ReaWeb_Close', 'ReaWeb_DevTools', 'ReaWeb_SetDocked', 'ReaWeb_IsDocked', 'ReaWeb_GetCapabilities'
  ];
  const api = Object.create(null);
  for (const name of methods) api[name] = (...args) => call(name, args);
  Object.defineProperty(window, 'reaper', { value: Object.freeze(api), enumerable: true });
  window.addEventListener('pagehide', () => {
    for (const item of pending.values()) {
      clearTimeout(item.timer);
      item.reject(failure('WINDOW_CLOSED', 'The WebView document was closed'));
    }
    pending.clear();
  });
})();
