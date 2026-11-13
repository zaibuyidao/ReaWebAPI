const logs = [];
const audio = { contexts: [], buffers: 0, nonzero: 0, states: [] };
const NativeAudioContext = window.AudioContext;
window.AudioContext = class extends NativeAudioContext {
  constructor(...args) { super(...args); audio.contexts.push(this);
    this.addEventListener('statechange',()=>audio.states.push({state:this.state,ms:performance.now(),focus:document.hasFocus(),visibility:document.visibilityState})); }
  createBufferSource() {
    const source = super.createBufferSource(), start = source.start;
    source.start = function (...args) {
      if (this.buffer) {
        ++audio.buffers;
        const samples = this.buffer.getChannelData(0);
        if (samples.some(sample => Math.abs(sample) > 0.00001)) ++audio.nonzero;
      }
      return start.apply(this, args);
    };
    return source;
  }
};
for (const key of ['error', 'warn']) {
  const original = console[key];
  console[key] = (...args) => { logs.push(args.map(arg => arg?.stack || String(arg)).join(' ').slice(0, 1500)); original.apply(console, args); };
}
addEventListener('error', event => logs.push(String(event.error?.stack || event.message).slice(0, 1500)));
addEventListener('unhandledrejection', event => logs.push(String(event.reason?.stack || event.reason)));
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const hash = bytes => { let n = 2166136261; for (const b of bytes) n = Math.imul(n ^ b, 16777619); return n >>> 0; };
const bounded = promise => Promise.race([promise, sleep(10000).then(() => { throw new Error('Core operation timeout'); })]);
const frame = canvas => new Promise(resolve => requestAnimationFrame(() => {
  const copy = document.createElement('canvas'); copy.width = canvas.width; copy.height = canvas.height;
  const context = copy.getContext('2d'); context.drawImage(canvas, 0, 0);
  resolve(context.getImageData(0, 0, copy.width, copy.height).data);
}));
const keyboardEvents = [];
for (const type of ['keydown', 'keyup']) addEventListener(type, event => {
  keyboardEvents.push({ type, keyCode: event.keyCode, trusted: event.isTrusted });
}, true);
const pressKey = async keyCode => {
  await reaper.ShowConsoleMsg(`CORE_KEY:${keyCode},1\n`); await sleep(200);
  await reaper.ShowConsoleMsg(`CORE_KEY:${keyCode},0\n`); await sleep(400);
};
const coreScreenshot = async (manager, label) => {
  const bytes = await bounded(manager.screenshot());
  if (label) {
    let binary = ''; for (const byte of bytes) binary += String.fromCharCode(byte);
    await reaper.ShowConsoleMsg(`CORE_IMAGE:${label}:${btoa(binary)}\n`);
  }
  const bitmap = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
  const canvas = document.createElement('canvas'); canvas.width = bitmap.width; canvas.height = bitmap.height;
  const context = canvas.getContext('2d'); context.drawImage(bitmap, 0, 0); bitmap.close();
  // The official controller fixture draws its latched button label on this line.
  if (canvas.width !== 480 || canvas.height < 40) throw new Error('Unexpected PSP controller screenshot dimensions');
  return context.getImageData(0, 30, 128, 10).data;
};
const color = (pixels, canvas) => Array.from(pixels.slice((Math.floor(canvas.height / 2) * canvas.width + Math.floor(canvas.width / 2)) * 4,
  (Math.floor(canvas.height / 2) * canvas.width + Math.floor(canvas.width / 2)) * 4 + 3));
let sent = false;
const report = async result => {
  if (sent) return; sent = true;
  await reaper.ShowConsoleMsg('CORE_REPORT:' + JSON.stringify({ core: EJS_core, threads: EJS_threads, ...result, logs }) + '\n');
};
window.EJS_onGameStart = async () => {
  try {
    const emulator = EJS_emulator, manager = emulator.gameManager;
    await sleep(2000);
    if (EJS_core === 'ppsspp' && audio.contexts.some(context=>['suspended','interrupted'].includes(context.state))) await pressKey(65);
    const initialFrame = manager.getFrameNum();
    const first = await bounded(frame(emulator.canvas));
    await sleep(500);
    const second = await bounded(frame(emulator.canvas));
    let behavior = null;
    if (EJS_core === 'ppsspp') {
      const scenario = window.acceptancePspScenario || 'cube';
      const baseline = hash(second);
      let pressed = baseline, released = baseline;
      let inputRestore = null;
      if (scenario === 'controller') {
        const key = emulator.controls[0][0].value;
        emulator.canvas.focus();
        await reaper.ShowConsoleMsg(`CORE_KEY:${key},1\n`); await sleep(400);
        pressed = hash(await coreScreenshot(manager, 'psp-pressed'));
        const inputSaved = manager.getState();
        await reaper.ShowConsoleMsg(`CORE_KEY:${key},0\n`); await sleep(400);
        const alternate = emulator.controls[0][8].value;
        await pressKey(alternate);
        released = hash(await coreScreenshot(manager, 'psp-alternate'));
        manager.loadState(inputSaved); await sleep(300);
        inputRestore = hash(await coreScreenshot(manager, 'psp-restored'));
      }
      manager.toggleMainLoop(0); await sleep(200);
      const saved = manager.getState(), pausedState = hash(saved);
      const pausedFrame = manager.getFrameNum();
      await sleep(300);
      const stillPausedState = hash(manager.getState());
      const stillPausedFrame = manager.getFrameNum();
      manager.toggleMainLoop(1); await sleep(600);
      const advanced = manager.getState(), advancedState = hash(advanced), advancedFrame = manager.getFrameNum();
      manager.toggleMainLoop(0); await sleep(200);
      manager.loadState(saved); manager.toggleMainLoop(1); await sleep(50);
      manager.toggleMainLoop(0); await sleep(100);
      const restored = manager.getState(), restoredState = hash(restored);
      let advancedDifference = 0, restoredDifference = 0;
      for (let i = 0; i < saved.length; ++i) {
        if (saved[i] !== advanced[i]) ++advancedDifference;
        if (saved[i] !== restored[i]) ++restoredDifference;
      }
      manager.toggleMainLoop(1); await sleep(200);
      behavior = { scenario, baseline, pressed, released, pausedState, stillPausedState, advancedState, restoredState,
        pausedFrame, stillPausedFrame, advancedFrame, advancedDifference, restoredDifference, inputRestore, keyboardEvents };
      behavior.stateRestored = scenario === 'controller' ? pressed !== released && inputRestore === pressed : null;
      behavior.passed = pausedFrame === stillPausedFrame && advancedFrame > pausedFrame &&
        (scenario === 'cube' ? hash(first) !== baseline : scenario === 'audio' ? audio.nonzero > 0 :
          behavior.stateRestored && keyboardEvents.filter(event => event.trusted).length >= 4);
    } else {
      const a = EJS_core === 'dosbox_pure' ? 65 : emulator.controls[0][8].value;
      const b = EJS_core === 'dosbox_pure' ? 66 : emulator.controls[0][0].value;
      if (EJS_core === 'dosbox_pure') emulator.changeSettingOption('keyboardInput', 'enabled');
      emulator.canvas.focus();
      await pressKey(a);
      const red = color(await bounded(frame(emulator.canvas)), emulator.canvas);
      const saved = manager.getState();
      await pressKey(b);
      const green = color(await bounded(frame(emulator.canvas)), emulator.canvas);
      manager.loadState(saved); await sleep(500);
      const restoredColor = color(await bounded(frame(emulator.canvas)), emulator.canvas);
      manager.toggleMainLoop(0); await sleep(300);
      const paused = hash(await bounded(frame(emulator.canvas)));
      const pausedState = hash(manager.getState());
      await sleep(300);
      const stillPaused = hash(await bounded(frame(emulator.canvas)));
      const stillPausedState = hash(manager.getState());
      manager.toggleMainLoop(1);
      const resumed = [];
      for (let i = 0; i < 6; ++i) { await sleep(100); resumed.push(hash(await bounded(frame(emulator.canvas)))); }
      behavior = { red, green, restoredColor, paused, stillPaused, pausedState, stillPausedState, resumed, keyboardEvents };
      behavior.passed = red[0] > red[1] * 2 && green[1] > green[0] * 2 && restoredColor[0] > restoredColor[1] * 2 &&
        pausedState === stillPausedState && resumed.some(value => value !== paused) &&
        keyboardEvents.filter(event => event.trusted).length >= 4;
    }
    const state = manager.getState();
    const restored = await bounded(frame(emulator.canvas));
    const activeGl2 = emulator.canvas.getContext('webgl2');
    const rendering = { webgl2: !!activeGl2, version: activeGl2?.getParameter(activeGl2.VERSION) ?? null,
      renderer: activeGl2?.getParameter(activeGl2.RENDERER) ?? null };
    if (!behavior?.passed) throw new Error('Core behavior failed: ' + JSON.stringify({behavior,
      focused:document.hasFocus(), visibility:document.visibilityState, paused:emulator.paused,
      frames:[initialFrame,manager.getFrameNum()], audio:audio.contexts.map(context=>({state:context.state,sampleRate:context.sampleRate})),
      window:await reaper.window.getState()}));
    await reaper.ShowConsoleMsg('CORE_BEHAVIOR:' + JSON.stringify({passed:true,behavior,rendering}) + '\n');
    const soak = [], soakStart = performance.now();
    while (performance.now() - soakStart < (window.acceptanceSoakSeconds || 0) * 1000) {
      await sleep(Math.min(30000, window.acceptanceSoakSeconds * 1000 - (performance.now() - soakStart)));
      const before = manager.getFrameNum(), audioBefore = audio.buffers;
      let after = before;
      for (let i = 0; i < 50 && (after <= before || audio.buffers <= audioBefore); ++i) {
        await sleep(100); after = manager.getFrameNum();
      }
      if (after <= before || audio.buffers <= audioBefore || logs.length) throw new Error('Core frame/audio progress stalled during soak');
      if (soak.length % 5 === 0) { const saved = manager.getState(); manager.loadState(saved); }
      soak.push({ seconds: (performance.now() - soakStart) / 1000, before, after,
        wasmBytes: emulator.Module.HEAPU8.length, audioBuffers: audio.buffers });
      await reaper.ShowConsoleMsg('CORE_PROGRESS:' + JSON.stringify(soak.at(-1)) + '\n');
    }
    await report({ passed: first.length > 100 && second.length > 100 && state.length > 0 &&
      (behavior?.passed ?? hash(first) !== hash(second)) && (EJS_core !== 'ppsspp' || rendering.webgl2) &&
      (emulator.Module.HEAPU8.buffer instanceof SharedArrayBuffer) === EJS_threads && logs.length === 0,
      started: emulator.started, sharedMemory: emulator.Module.HEAPU8.buffer instanceof SharedArrayBuffer,
      screenshotBytes: [first.length, second.length, restored.length], hashes: [hash(first), hash(second), hash(restored)],
      stateBytes: state.length, webgl2: emulator.supportsWebgl2,
      soak,
      behavior,
      rendering,
      audio: { contexts: audio.contexts.map(context => ({ state: context.state, sampleRate: context.sampleRate })), buffers: audio.buffers, nonzero: audio.nonzero },
      diagnostics: await reaper.debug.getDiagnostics() });
  } catch (error) { await report({ passed: false, error: String(error), frame:window.EJS_emulator?.gameManager?.getFrameNum(),
    audio:audio.contexts.map(context=>({state:context.state,sampleRate:context.sampleRate})), audioStates:audio.states,
    focused:document.hasFocus(), keyboardEvents }); }
};
setTimeout(() => report({ passed: false, error: 'Core acceptance timed out', text: document.body.innerText.slice(0, 1000) }), 90000 + (window.acceptanceSoakSeconds || 0) * 1000);
