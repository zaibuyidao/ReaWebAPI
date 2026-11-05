#pragma once

namespace reaweb {
struct ShortcutKey { int key = 0, modifiers = 0; bool down = false, repeat = false; int character = 0; };
inline const char* shortcut_keyboard_script = R"JS(
(() => {
  if (window !== window.top) return;
  const post = window.chrome?.webview
    ? message => window.chrome.webview.postMessage(message)
    : message => window.webkit.messageHandlers.reaweb.postMessage(message);
  const roots = new WeakMap(), attach = Element.prototype.attachShadow;
  Object.defineProperty(Element.prototype, 'attachShadow', {
    ...Object.getOwnPropertyDescriptor(Element.prototype, 'attachShadow'),
    value: function(options) { const root = Reflect.apply(attach, this, [options]); roots.set(this, root); return root; }
  });
  let enabled = false, documentId = '', composing = false;
  const held = new Map(), cancelled = new Set();
  const editable = event => {
    if (document.designMode === 'on') return true;
    const nodes = event.composedPath();
    for (let active = document.activeElement; active; active = (roots.get(active) || active.shadowRoot)?.activeElement) nodes.push(active);
    return nodes.some(node => node?.isContentEditable || node?.tagName === 'IFRAME' ||
      (node?.tagName === 'TEXTAREA' && !node.disabled && !node.readOnly) ||
      (node?.tagName === 'INPUT' && !node.disabled && !node.readOnly &&
       !['button','submit','reset','checkbox','radio','range','color','file','hidden'].includes(node.type)));
  };
  const keyCode = event => {
    const code = event.code;
    if (/^Key[A-Z]$/.test(code)) return (event.key?.length === 1 && /^[a-z]$/i.test(event.key) ? event.key.toUpperCase() : code.slice(3)).charCodeAt(0);
    if (/^Digit[0-9]$/.test(code)) return +code.slice(5) + 48;
    if (/^Numpad[0-9]$/.test(code)) return +code.slice(6) + 96;
    if (/^F([1-9]|1[0-9]|2[0-4])$/.test(code)) return +code.slice(1) + 111;
    return ({Backspace:8,Tab:9,Enter:13,NumpadEnter:13,Escape:27,Space:32,PageUp:33,PageDown:34,End:35,Home:36,
      ArrowLeft:37,ArrowUp:38,ArrowRight:39,ArrowDown:40,Insert:45,Delete:46,NumpadMultiply:106,NumpadAdd:107,
      NumpadSubtract:109,NumpadDecimal:110,NumpadDivide:111,Semicolon:186,Equal:187,Comma:188,Minus:189,
      Period:190,Slash:191,Backquote:192,BracketLeft:219,Backslash:220,BracketRight:221,Quote:222})[code] || 0;
  };
  const send = (key, down, repeat = false) => post('__reawebShortcut:' + JSON.stringify({document:documentId,...key,down,repeat}));
  const stop = event => { event.preventDefault(); event.stopImmediatePropagation(); };
  const release = () => {
    for (const [code, key] of held) if (key) { send(key, false); cancelled.add(code); }
    held.clear();
  };
  Object.defineProperty(window, '__reawebSetShortcutRouting', {value: (value, document) => {
    if (enabled && (!value || documentId !== document)) release();
    enabled = value === true; documentId = document;
  }});
  Object.defineProperty(window, '__reawebReleaseShortcut', {value: (key, character) => {
    for (const [code, owner] of held) if (owner && (key ? owner.key === key : owner.character === character)) {
      send(owner, false); held.delete(code); cancelled.add(code);
    }
  }});
  addEventListener('compositionstart', () => { composing = true; }, true);
  addEventListener('compositionend', () => { composing = false; }, true);
  addEventListener('keydown', event => {
    if (!event.isTrusted) return;
    if (cancelled.has(event.code)) {
      if (event.repeat) { stop(event); return; }
      cancelled.delete(event.code);
    }
    // Some native Command combinations omit the WebView keyup.
    if (!event.repeat && held.get(event.code)) { send(held.get(event.code), false); held.delete(event.code); }
    if (!held.has(event.code)) {
      const key = keyCode(event);
      if (!enabled) return;
      held.set(event.code, key && !event.repeat && !composing && !event.isComposing && event.keyCode !== 229 && !editable(event)
        ? {key, character:!event.altKey && event.key?.length === 1 ? event.key.charCodeAt(0) : 0,
           modifiers:(event.shiftKey?4:0)|(event.ctrlKey?8:0)|(event.altKey?16:0)|(event.metaKey?32:0)} : null);
    }
    const owner = held.get(event.code);
    if (owner) { stop(event); send(owner, true, event.repeat); }
  }, true);
  addEventListener('keyup', event => {
    if (!event.isTrusted) return;
    const owner = held.get(event.code);
    if (owner) { stop(event); send(owner, false); }
    else if (cancelled.has(event.code)) stop(event);
    held.delete(event.code); cancelled.delete(event.code);
  }, true);
  addEventListener('blur', () => { release(); composing = false; });
  addEventListener('pagehide', release);
})();
)JS";
}
