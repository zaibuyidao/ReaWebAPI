import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
const source = await readFile(new URL('../examples/integrations/video-preview/native-surface.js', import.meta.url), 'utf8');
const { surfaceLayout, createSurfaceLayout, nativePresentationRate } = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);

test('Native presentation metrics preserve unavailable counters without inventing a draw rate', async () => {
  const unavailable = { presentationCounter: 'dxgi-observed', presentationAvailable: false, presentedFrames: null, presentationFPS: null };
  assert.equal(nativePresentationRate(unavailable, 60, 1), null);
  assert.equal(nativePresentationRate({ presentedFrames: null }, 60, 1), null);
  assert.equal(nativePresentationRate({ presentedFrames: 120, presentationFPS: null }, 60, 1), null);
  assert.equal(nativePresentationRate({ presentedFrames: 120 }, null, 1), null);
  assert.equal(nativePresentationRate({ presentedFrames: 120 }, 60, 1), 60);
  assert.equal(nativePresentationRate({ presentationCounter: 'dxgi-observed', presentationAvailable: true, presentationFPS: 59.94 }, null, 1), 59.94);
  const app = await readFile(new URL('../examples/integrations/video-preview/app.js', import.meta.url), 'utf8');
  const metrics = [];
  const context = vm.createContext({ metricData: { targetUpdateRate: 60, actualFPS: 60, width: 3840, height: 2160 },
    canvas: { getBoundingClientRect: () => ({ width: 1280, height: 720 }) }, stream: { readablePixels: false },
    source: { width: 3840, height: 2160 }, devicePixelRatio: 1, drawRate: null, nativeMetrics: null, t: key => key,
    $: () => ({ replaceChildren: (...nodes) => metrics.push(...nodes) }),
    document: { createElement: () => ({ append(...children) { this.children = children; } }), createTextNode: value => value }
  });
  vm.runInContext(app.slice(app.indexOf('function renderMetrics()'), app.indexOf('\ntranslate();', app.indexOf('function renderMetrics()'))) + '\nrenderMetrics();', context);
  assert.equal(metrics[3].children[0].textContent, 'nativeFps');
  assert.equal(metrics[3].children[1], '60.00 / 60.0 / —');
  assert.equal(metrics[3].title, undefined);
  vm.runInContext("stream.info={renderer:'native-d3d11'};drawRate=56.7;renderMetrics();", context);
  assert.equal(metrics[7].children[0].textContent, 'nativeObservedFps');
  assert.equal(metrics[7].children[1], '60.00 / 60.0 / 56.7');
  assert.equal(metrics[7].title, 'nativeObservedHint');
  vm.runInContext("stream.info={renderer:'native-metal'};nativeMetrics={presentationCounter:'dxgi-observed'};renderMetrics();", context);
  assert.equal(metrics[11].children[0].textContent, 'nativeObservedFps');
});

test('Native video layout keeps CSS coordinates and clips scrolling against the viewport', () => {
  const canvas = { getBoundingClientRect: () => ({ left: -300, top: -100, width: 1920, height: 1080 }) };
  const stage = { getBoundingClientRect: () => ({ left: 16, top: 60 }), clientLeft: 1, clientTop: 1, clientWidth: 768, clientHeight: 432 };
  assert.deepEqual(surfaceLayout(canvas, stage, true, { width: 800, height: 480 }), {
    rect: { x: -300, y: -100, width: 1920, height: 1080 }, clip: { x: 17, y: 61, width: 768, height: 419 }, preserveAspect: true
  });
  stage.getBoundingClientRect = () => ({ left: 900, top: 700 });
  assert.deepEqual(surfaceLayout(canvas, stage, false, { width: 800, height: 480 }).clip,
    { x: 800, y: 480, width: 0, height: 0 });
});

test('Native layout coalesces resize and scroll while preserving the newest pending update', async () => {
  const scheduled = new Map(); let serial = 0, x = 10, resolveFirst;
  globalThis.innerWidth = 800; globalThis.innerHeight = 600;
  globalThis.requestAnimationFrame = callback => { scheduled.set(++serial, callback); return serial; };
  globalThis.cancelAnimationFrame = id => scheduled.delete(id);
  const pump = () => { for (const [id, callback] of [...scheduled]) { scheduled.delete(id); callback(); } };
  const calls = [], errors = [];
  const surface = { closed: false, setLayout: value => {
    calls.push(value); return calls.length === 1 ? new Promise(resolve => { resolveFirst = resolve; }) : Promise.resolve(true);
  } };
  const canvas = { getBoundingClientRect: () => ({ left: x, top: 20, width: 640, height: 360 }) };
  const stage = { getBoundingClientRect: () => ({ left: 0, top: 0 }), clientLeft: 0, clientTop: 0, clientWidth: 800, clientHeight: 600 };
  const layout = createSurfaceLayout(surface, canvas, stage, error => errors.push(error));
  layout.update(true); x = 11; layout.update(true); pump();
  assert.equal(calls.length, 1); assert.equal(calls[0].rect.x, 11);
  x = 12; layout.update(true); x = 13; layout.update(false); pump();
  assert.equal(calls.length, 1);
  resolveFirst(true); await new Promise(setImmediate);
  assert.equal(calls.length, 2); assert.equal(calls[1].rect.x, 13); assert.equal(calls[1].preserveAspect, false);
  layout.update(false); pump(); await new Promise(setImmediate); assert.equal(calls.length, 2);
  x = 14; layout.update(true); layout.dispose(); pump();
  assert.equal(calls.length, 2); assert.deepEqual(errors, []);
});

test('Demo native initialization failures restore the requested canvas profile and menu controls', async () => {
  const app = (await readFile(new URL('../examples/integrations/video-preview/app.js', import.meta.url), 'utf8')).replace(/^import .*;\r?\n/gm, '');
  for (const platform of ['macos', 'windows']) for (const code of ['VIDEO_FORMAT_UNSUPPORTED', 'VIDEO_RENDERER_UNAVAILABLE']) for (const mode of ['fit', 'source', 'custom']) {
    const classes = () => { const values = new Set(); return { add: value => values.add(value), remove: value => values.delete(value), contains: value => values.has(value), toggle: (value, on) => on ? values.add(value) : values.delete(value) }; };
    const element = () => ({ value: '', checked: false, disabled: false, hidden: false, style: {}, classList: classes(),
      clientLeft: 0, clientTop: 0, clientWidth: 640, clientHeight: 360, setAttribute() {},
      getBoundingClientRect: () => ({ left: 0, top: 60, width: 640, height: 360 }), replaceChildren() {} });
    const nodes = new Map(), get = id => { if (!nodes.has(id)) nodes.set(id, element()); return nodes.get(id); };
    let canvas = Object.assign(element(), { width: 1, height: 1 });
    canvas.cloneNode = () => ({ ...canvas, style: {} }); canvas.replaceWith = replacement => { canvas = replacement; };
    get('resolution').value = mode; get('aspect').checked = true; get('width').value = '1920'; get('height').value = '1080';
    const documentEvents = {}, timers = new Map(), nativeOptions = [], byteOptions = [], resizes = [], errorMessages = [];
    let timer = 0, cleanup;
    const root = { dataset: {}, classList: classes() };
    const document = { documentElement: root, hidden: false, getElementById: get, querySelector: () => canvas,
      querySelectorAll: () => [], addEventListener: (name, callback) => { documentEvents[name] = callback; } };
    const renderer = { kind: 'test-canvas', dropped: 0, clear() {}, dispose() {}, present() {}, push() {}, resize: (w, h) => resizes.push([w, h]) };
    const source = { available: true, width: 3840, height: 2160, nativeFormat: 'i420', colorMatrix: 'bt709', colorRange: 'limited', colorConversion: platform === 'macos' ? 'reaper-neon' : 'reaper-sse' };
    const reaper = { lifecycle: { ready: Promise.resolve(), on: async (_, callback) => { cleanup = callback; } },
      window: { setIconVisible: async () => {} }, video: {
        getCapabilities: async () => ({ available: true, platform, resolutionModes: ['source', 'fit', 'custom'], formats: ['rgba8', 'i420'], nativePresentation: { available: true, backend: platform === 'macos' ? 'metal' : 'd3d11' } }),
        getSourceInfo: async () => source,
        openSurface: async options => { nativeOptions.push({ ...options }); throw Object.assign(new Error('Native initialization failed'), { code }); },
        openStream: async options => { byteOptions.push({ ...options }); return {
          info: { width: options.width || 3840, height: options.height || 2160 }, closed: false,
          close() { this.closed = true; return Promise.resolve(); }, on() {}, latest: () => null
        }; }
      } };
    const context = vm.createContext({ document, window: { addEventListener() {} }, reaper,
      localStorage: { getItem: () => null, setItem() {} }, devicePixelRatio: 2, innerWidth: 800, innerHeight: 600,
      performance, console: { error: value => errorMessages.push(value) },
      setTimeout: callback => { timers.set(++timer, callback); return timer; }, clearTimeout: id => timers.delete(id),
      setInterval: () => 1, clearInterval() {}, ResizeObserver: class { observe() {} disconnect() {} },
      createVideoRenderer: async () => renderer, createSurfaceLayout: () => { throw new Error('Failed native surface must not bind a layout'); }
    });
    vm.runInContext(app, context); await new Promise(setImmediate);
    assert.equal(nativeOptions.length, 1, `${code} ${mode}: native attempt`);
    for (const [id, callback] of [...timers]) { timers.delete(id); callback(); }
    await new Promise(setImmediate);
    assert.equal(byteOptions.length, 1, `${code} ${mode}: canvas fallback`);
    assert.equal(byteOptions[0].resolution, mode);
    if (mode === 'fit') {
      assert.equal(nativeOptions[0].resolution, 'source'); assert.equal(nativeOptions[0].width, undefined);
      assert.equal(byteOptions[0].format, undefined); assert.deepEqual([byteOptions[0].width, byteOptions[0].height], [1280, 720]);
    } else if (mode === 'custom') assert.deepEqual([byteOptions[0].width, byteOptions[0].height], [1920, 1080]);
    else assert.equal(byteOptions[0].format, 'i420');
    assert.equal(root.classList.contains('native-video'), false);
    assert.equal(get('resolution').value, mode); assert.equal(get('resolution').disabled, false);
    assert.equal(resizes.length, 1); assert.deepEqual(errorMessages, []);
    documentEvents.scroll(); assert.equal(byteOptions.length, 1);
    await cleanup();
  }
});

test('Native windows share one source profile while aspect and viewport changes remain display-only', async () => {
  const app = await readFile(new URL('../examples/integrations/video-preview/app.js', import.meta.url), 'utf8');
  const optionsAndMatch = app.slice(app.indexOf('function options()'), app.indexOf('async function detach('));
  const reconcile = app.slice(app.indexOf('async function reconcile()'), app.indexOf('function schedule()'));
  const sharedProfile = { source: 'project', resolution: 'source', preserveAspect: true, updateRate: 'auto', format: 'i420' };
  const windows = [true, false].map((aspect, index) => {
    const controls = { original: { checked: false }, resolution: { value: 'fit' }, aspect: { checked: aspect } };
    const layouts = [];
    const state = { $: name => controls[name], preferNative: () => true, devicePixelRatio: 2,
      stage: { clientWidth: index ? 640 : 1280, clientHeight: index ? 360 : 720 },
      stream: { closed: false, info: { width: 3840, height: 2160, dynamicResolution: true } }, source: { width: 3840, height: 2160 },
      profileKey: JSON.stringify(sharedProfile), wanted: true, closing: false, busy: false, dirty: false, document: { hidden: false },
      layout: () => layouts.push(controls.aspect.checked),
      detach: () => { throw new Error('Display-only changes must not reopen the source'); },
      report: error => { throw error; }
    };
    const context = vm.createContext(state); vm.runInContext(optionsAndMatch + reconcile, context);
    return { controls, layouts, context, state };
  });
  for (const window of windows) {
    assert.equal(vm.runInContext('options().preserveAspect', window.context), true);
    await vm.runInContext('reconcile()', window.context);
    window.controls.aspect.checked = !window.controls.aspect.checked;
    window.state.stage.clientWidth += 100;
    await vm.runInContext('reconcile()', window.context);
    assert.deepEqual(window.layouts, [!window.controls.aspect.checked, window.controls.aspect.checked]);
    assert.equal(vm.runInContext('options().preserveAspect', window.context), true);
    window.controls.resolution.value = 'source'; await vm.runInContext('reconcile()', window.context);
    assert.equal(window.layouts.length, 3);
    window.state.source = { width: 1280, height: 720 };
    await vm.runInContext('reconcile()', window.context);
    assert.equal(window.layouts.length, 4, 'A source size change keeps the dynamic connection');
  }
});

test('Demo falls back after asynchronous renderer failure but keeps native presentation after normal close', async () => {
  const app = await readFile(new URL('../examples/integrations/video-preview/app.js', import.meta.url), 'utf8');
  const functions = app.slice(app.indexOf('function options()'), app.indexOf('async function detach(')) +
    app.slice(app.indexOf('async function reconcile()'), app.indexOf('function schedule()'));
  for (const errorCode of ['VIDEO_RENDERER_UNAVAILABLE', 'VIDEO_PROJECT_CHANGED', null]) {
    const nativeOptions = [], byteOptions = [], events = new Map(), reports = [];
    const controls = { original: { checked: false }, resolution: { value: 'fit' }, aspect: { checked: true } };
    const state = { $: key => controls[key], stage: { clientWidth: 640, clientHeight: 360 }, devicePixelRatio: 2,
      nativeUnavailable: false, nativeLayout: null, nativeMetrics: null, renderer: null, rendererFormat: 'rgba8', i420Unavailable: false,
      stream: null, profileKey: '', source: { width: 3840, height: 2160 }, wanted: true, closing: false, busy: false, dirty: false,
      capabilities: { formats: ['rgba8', 'i420'] }, document: { hidden: false, documentElement: { classList: { add() {}, remove() {} } } },
      canvas: {}, performance, layout() {}, clear() {}, discardRetainedFrame() {}, status() {}, report: error => reports.push(error.code),
      createSurfaceLayout: () => ({ update() {}, dispose() {} }),
      reaper: { video: {
        getSourceInfo: async () => ({ available: true, nativeFormat: 'i420', colorMatrix: 'bt709', colorRange: 'limited', colorConversion: 'reaper-sse' }),
        openSurface: async options => { nativeOptions.push({ ...options }); return { closed: false, readablePixels: false,
          info: { width: 3840, height: 2160 }, on: (event, callback) => events.set(event, callback), close: async () => {} }; }
      } }
    };
    const context = vm.createContext(Object.assign(state, { byteOptions }));
    vm.runInContext(`const preferNative=()=>!nativeUnavailable;
      async function detach(){nativeLayout?.dispose();nativeLayout=null;const old=stream;stream=null;profileKey='';await old?.close();}
      async function selectRenderer(format){rendererFormat=format;renderer={resize(){},clear(){},present(){},push(){},openStream:async options=>{byteOptions.push({...options});return {closed:false,info:{width:options.width,height:options.height},on(){},close:async()=>{}};}};}
      ` + functions, context);
    await vm.runInContext('reconcile()', context);
    assert.equal(nativeOptions.length, 1); assert.equal(byteOptions.length, 0);
    if (errorCode) events.get('error')({ code: errorCode, message: 'Native presentation ended' });
    events.get('close')();
    await vm.runInContext('reconcile()', context);
    if (errorCode === 'VIDEO_RENDERER_UNAVAILABLE') {
      assert.equal(nativeOptions.length, 1); assert.equal(byteOptions.length, 1);
      assert.deepEqual([byteOptions[0].resolution, byteOptions[0].width, byteOptions[0].height], ['fit', 1280, 720]);
    } else { assert.equal(nativeOptions.length, 2); assert.equal(byteOptions.length, 0); }
    assert.deepEqual(reports, errorCode ? [errorCode] : []);
  }
});

test('Canvas fit reconnect keeps the displayed canvas until the replacement draws', async () => {
  const app = await readFile(new URL('../examples/integrations/video-preview/app.js', import.meta.url), 'utf8');
  const functions = app.slice(app.indexOf('function discardRetainedFrame()'), app.indexOf('function layout()')) +
    app.slice(app.indexOf('async function detach('), app.indexOf('async function reconcile()'));
  const ready = new Set(['ready']), classes = new Set();
  const previous = {
    classList: classes, cloneNode: () => ({}), before() {}, remove() { this.removed = true; }
  };
  const oldRenderer = { clear() { assert.fail('The retained canvas must keep its pixels'); }, dispose() { this.disposed = true; } };
  let onDraw, closed = false;
  const context = vm.createContext({
    retainedFrame: undefined, canvas: previous, renderer: oldRenderer, rendererFormat: 'rgba8', painted: 0,
    nativeLayout: null, nativeMetrics: null, profileKey: '', stage: { classList: { contains: key => ready.has(key),
      add: key => ready.add(key), remove: key => ready.delete(key) } },
    stream: { close: async () => { closed = true; } },
    document: { documentElement: { classList: { remove() {} } } },
    capabilities: { platform: 'linux' },
    createVideoRenderer: async (_, options) => { onDraw = options.onDraw; return {}; }
  });
  vm.runInContext(functions, context);
  await vm.runInContext("detach(true).then(() => selectRenderer('rgba8'))", context);
  assert.ok(closed && classes.has('retained-frame'));
  assert.ok(!previous.removed && !oldRenderer.disposed && !ready.has('ready'));
  onDraw();
  assert.ok(previous.removed && oldRenderer.disposed && ready.has('ready'));
});
