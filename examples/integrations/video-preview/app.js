import { createVideoRenderer } from './video-renderer.js';
import { createSurfaceLayout, nativePresentationRate } from './native-surface.js';
const $ = id => document.getElementById(id);
const messages = {
  en: {
    projectMonitor: 'PROJECT MONITOR', title: 'Video preview', options: 'Options', display: 'Display', theme: 'Appearance', dark: 'Dark', light: 'Light',
    fullscreen: 'Fullscreen', doubleClick: 'Double-click video', aspect: 'Keep video aspect ratio', original: 'Size window to original video',
    placeholder: 'Your project, in focus.', placeholderHint: 'Waiting for the final project video output.', exitFullscreen: 'Exit fullscreen · Esc',
    output: 'OUTPUT', fit: 'Fit viewport', sourceMode: 'Original resolution', custom: 'Custom resolution', width: 'WIDTH', height: 'HEIGHT', apply: 'Apply',
    hint: 'Auto frame rate · Final project output', details: 'Technical details', connecting: 'Connecting…',
    waiting: 'Waiting for REAPER to decode the final frame…', connected: 'Connected · waiting for the final project frame',
    ready: 'Preparing preview…', live: 'Live · double-click the video for fullscreen',
    originalStatus: 'Original pixels · scroll when the image exceeds the viewport', source: 'SOURCE', stream: 'STREAM', canvas: 'CANVAS BOX',
    fps: 'FPS · TARGET / CAPTURE / RECEIVE / DRAW', nativeFps: 'FPS · TARGET / CAPTURE / PRESENT', nativeObservedFps: 'FPS · TARGET / CAPTURE / OBSERVED PRESENT', nativeObservedHint: 'DXGI observations are a lower bound and may omit frames that were displayed.', nativeBox: 'DISPLAY AREA', pending: 'Waiting', stage: 'Video preview. Double-click to toggle fullscreen.',
    fullscreenUnavailable: 'Fullscreen is unavailable in this WebView. Use a floating preview window or a supported host.',
    originalUnavailable: 'Original size requires a valid frame within the output pixel budget.', install: 'Install a compatible ReaWebAPI v0.3.9.33 video backend.'
  },
  cn: {
    projectMonitor: '工程视频', title: '视频预览', options: '选项', display: '显示', theme: '外观', dark: '深色', light: '浅色',
    fullscreen: '全屏', doubleClick: '双击视频切换', aspect: '保持视频宽高比', original: '将窗口调整到原始视频大小',
    placeholder: '预览工程画面', placeholderHint: '正在等待工程的最终视频输出。', exitFullscreen: '退出全屏 · Esc',
    output: '输出', fit: '适应显示区域', sourceMode: '原始分辨率', custom: '自定义分辨率', width: '宽度', height: '高度', apply: '应用',
    hint: '自动跟随帧率 · 工程最终输出', details: '技术详情', connecting: '正在连接…',
    waiting: '正在等待 REAPER 解码最终画面…', connected: '已连接，正在等待工程画面',
    ready: '正在准备预览…', live: '预览中，双击视频可切换全屏',
    originalStatus: '原始像素显示，画面超出显示区域时可滚动查看', source: '源画面', stream: '视频流', canvas: '画布区域',
    fps: '帧率 · 目标 / 捕获 / 接收 / 绘制', nativeFps: '帧率 · 目标 / 捕获 / 呈现', nativeObservedFps: '帧率 · 目标 / 捕获 / 观察呈现', nativeObservedHint: 'DXGI 观察计数为下限，可能漏计已显示的帧。', nativeBox: '显示区域', pending: '等待画面', stage: '视频预览，双击切换全屏。',
    fullscreenUnavailable: '当前 WebView 无法全屏，请使用浮动预览窗口或支持全屏的宿主。',
    originalUnavailable: '原始大小需要有效的视频画面，且尺寸不能超出输出像素预算。', install: '请安装兼容的 ReaWebAPI v0.3.9.33 视频后端。'
  }
};
let language = 'en', statusKey = 'connecting', statusError;
try { language = localStorage.getItem('video-preview-language') === 'cn' ? 'cn' : 'en'; } catch {}
const t = key => messages[language][key];
function status(key) { statusKey = key; statusError = null; $('status').classList.remove('error'); $('status').textContent = t(key); }
function translate() {
  document.documentElement.lang = language === 'cn' ? 'zh-CN' : 'en'; document.title = t('title');
  document.querySelectorAll('[data-i18n]').forEach(element => { element.textContent = t(element.dataset.i18n); });
  $('en').setAttribute('aria-pressed', String(language === 'en')); $('cn').setAttribute('aria-pressed', String(language === 'cn'));
  $('stage').setAttribute('aria-label', t('stage')); document.querySelector('canvas').setAttribute('aria-label', t('title')); $('metrics').setAttribute('aria-label', t('details'));
  if (statusError) report(statusError); else $('status').textContent = t(statusKey);
}
for (const value of ['en', 'cn']) $(value).onclick = () => { language = value; try { localStorage.setItem('video-preview-language', value); } catch {} translate(); renderMetrics(); };
try { $('theme').value = localStorage.getItem('video-preview-theme') === 'light' ? 'light' : 'dark'; } catch {}
document.documentElement.dataset.theme = $('theme').value;
$('theme').onchange = () => { document.documentElement.dataset.theme = $('theme').value; try { localStorage.setItem('video-preview-theme', $('theme').value); } catch {} };

let canvas = document.querySelector('canvas');
const stage = $('stage');
let renderer, rendererFormat = 'rgba8', i420Unavailable = false, outputWidth = canvas.width, outputHeight = canvas.height;
let nativeUnavailable = false, nativeLayout, nativeMetrics = null, previousPresented = 0;
const preferNative = () => capabilities?.nativePresentation?.available && !nativeUnavailable && typeof reaper.video.openSurface === 'function';
let stream, wanted = false, busy = false, dirty = false, closing = false, poll, resizeTimer, capabilities, profileKey = '', previousMode = 'fit', savedWindow;
let sequenceGaps = 0, lastSequence, received = 0, painted = 0, sampledAt = performance.now(), source = { width: 0, height: 0 }, drawRate = 0, receiveRate = 0;
const report = error => { statusError = error; $('status').classList.toggle('error', error.code !== 'VIDEO_FRAME_PENDING'); $('status').textContent = error.code === 'VIDEO_FRAME_PENDING' ? t('waiting') : `${error.code || 'VIDEO_ERROR'}: ${error.messageKey ? t(error.messageKey) : error.message}`; };
function clear() { renderer?.clear(); stage.classList.remove('ready');  }
function layout() {
  const original = $('original').checked;
  canvas.style.width = original ? `${outputWidth / devicePixelRatio}px` : '100%';
  canvas.style.height = original ? `${outputHeight / devicePixelRatio}px` : '100%';
  canvas.style.objectFit = rendererFormat === 'i420' ? 'fill' : $('aspect').checked ? 'contain' : 'fill';
  canvas.style.margin = original ? '0' : 'auto';
  canvas.style.imageRendering = original ? 'pixelated' : 'auto';
  if (original) {
    const box = stage.getBoundingClientRect(), scale = devicePixelRatio;
    const x = Math.max(0, (stage.clientWidth - outputWidth / scale) / 2);
    const y = Math.max(0, (stage.clientHeight - outputHeight / scale) / 2);
    canvas.style.marginLeft = `${Math.round((box.left + x) * scale) / scale - box.left}px`;
    canvas.style.marginTop = `${Math.round((box.top + y) * scale) / scale - box.top}px`;
  }
  $('resolution').disabled = original;
  $('custom').hidden = $('resolution').value !== 'custom' || original;
  if (nativeLayout) nativeLayout.update($('aspect').checked);
  else renderer?.present?.(original ? outputWidth : Math.max(1, Math.round(canvas.clientWidth * devicePixelRatio)),
    original ? outputHeight : Math.max(1, Math.round(canvas.clientHeight * devicePixelRatio)), $('aspect').checked);
}
function options() {
  const requested = $('original').checked ? 'source' : $('resolution').value;
  const resolution = preferNative() && requested === 'fit' ? 'source' : requested;
  const value = { source: 'project', resolution, preserveAspect: preferNative() && resolution === 'source' ? true : $('aspect').checked, updateRate: 'auto' };
  if (resolution === 'fit') {
    value.width = Math.max(1, Math.min(8192, Math.round(stage.clientWidth * devicePixelRatio)));
    value.height = Math.max(1, Math.min(8192, Math.round(stage.clientHeight * devicePixelRatio)));
  } else if (resolution === 'custom') { value.width = Number($('width').value); value.height = Number($('height').value); }
  return value;
}
function sameOutput(next) {
  if (!stream || stream.closed) return false;
  const current = JSON.parse(profileKey);
  if (current.resolution !== next.resolution || current.preserveAspect !== next.preserveAspect) return false;
  if (next.resolution === 'source') return stream.info.width === source.width && stream.info.height === source.height;
  let w = next.width, h = next.height;
  if (next.resolution === 'fit' && source.width && source.height) {
    if (next.preserveAspect) { const scale = Math.min(1, w / source.width, h / source.height); w = Math.max(1, Math.floor(source.width * scale)); h = Math.max(1, Math.floor(source.height * scale)); }
    else { w = Math.min(w, source.width); h = Math.min(h, source.height); }
  }
  return stream.info.width === w && stream.info.height === h;
}
async function detach() {
  nativeLayout?.dispose(); nativeLayout = null; nativeMetrics = null;
  const old = stream; stream = null; profileKey = ''; clear();
  document.documentElement.classList.remove('native-video');
  await old?.close();
}
async function selectRenderer(format) {
  if (renderer && rendererFormat === format) return;
  if (renderer) {
    renderer.dispose(); renderer = null;
    const replacement = canvas.cloneNode(false); canvas.replaceWith(replacement); canvas = replacement;
  }
  rendererFormat = format;
  try {
    renderer = await createVideoRenderer(canvas, { platform: capabilities.platform, format, onDraw: () => ++painted,
      onError: error => { wanted = false; report(error); reconcile().catch(report); } });
  } catch (error) {
    if (format !== 'i420') throw error;
    i420Unavailable = true;
    const replacement = canvas.cloneNode(false); canvas.replaceWith(replacement); canvas = replacement;
    await selectRenderer('rgba8');
  }
}
async function reconcile() {
  if (closing) return;
  if (busy) { dirty = true; return; }
  busy = true;
  try {
    if (!wanted || document.hidden) { await detach(); return; }
    const nextOptions = options();
    if (sameOutput(nextOptions)) { layout(); return; }
    await detach();
    let format = 'rgba8';
    if (capabilities.formats?.includes('i420') && nextOptions.resolution === 'source' && !i420Unavailable) {
      const info = await reaper.video.getSourceInfo();
      if (!info.available && info.error === 'VIDEO_FRAME_PENDING')
        throw Object.assign(new Error('Waiting for the final project frame'), { code: info.error });
      if (info.nativeFormat === 'i420' && info.colorMatrix === 'bt709' && info.colorRange === 'limited' &&
          ['reaper-neon', 'reaper-sse'].includes(info.colorConversion)) format = 'i420';
    }
    const native = preferNative();
    if (!native) await selectRenderer(format);
    if ((native ? format : rendererFormat) === 'i420') nextOptions.format = 'i420';
    const connect = async () => {
      const deadline = performance.now() + 5000;
      for (;;) {
        let connection;
        try {
          connection = await (native ? reaper.video.openSurface(nextOptions) : renderer.openStream ? renderer.openStream(nextOptions) : reaper.video.openStream(nextOptions));
          if (!native) renderer.resize(connection.info.width, connection.info.height, connection.info);
          return connection;
        } catch (error) {
          if (connection) await connection.close().catch(() => {});
          if (error.code !== 'VIDEO_FRAME_PENDING' || performance.now() >= deadline || closing || !wanted || document.hidden) throw error;
          await new Promise(resolve => setTimeout(resolve, 100));
        }
      }
    };
    let next;
    try { next = await connect(); }
    catch (error) {
      if (native) {
        if (['VIDEO_PRESENTATION_UNAVAILABLE', 'VIDEO_FORMAT_UNSUPPORTED', 'VIDEO_RENDERER_UNAVAILABLE', 'VIDEO_RENDER_UNSUPPORTED', 'VIDEO_SURFACE_UNSUPPORTED', 'VIDEO_PRESENTATION_UNSUPPORTED', 'HOST_UNAVAILABLE', 'UNKNOWN_METHOD'].includes(error.code)) {
          nativeUnavailable = true; dirty = true; return;
        }
        throw error;
      }
      if (rendererFormat !== 'i420' || !['VIDEO_FORMAT_UNSUPPORTED', 'VIDEO_FORMAT_CHANGED', 'VIDEO_PROFILE_CONFLICT', 'UNSUPPORTED_FORMAT', 'VIDEO_RENDER_UNSUPPORTED', 'VIDEO_RENDER_LIMIT'].includes(error.code)) throw error;
      delete nextOptions.format; await selectRenderer('rgba8');
      next = await connect();
    }
    if (closing || !wanted || document.hidden) { await next.close(); return; }
    stream = next; profileKey = JSON.stringify(nextOptions);
    if (native) {
      if (renderer) {
        renderer.dispose(); renderer = null;
        const replacement = canvas.cloneNode(false); canvas.replaceWith(replacement); canvas = replacement;
      }
      rendererFormat = format;
      if (nextOptions.resolution === 'source') source = { width: next.info.width, height: next.info.height };
      document.documentElement.classList.add('native-video');
      nativeLayout = createSurfaceLayout(next, canvas, stage, error => { if (stream === next) report(error); });
      previousPresented = 0;
    }
    outputWidth = next.info.width; outputHeight = next.info.height; layout();
    sequenceGaps = 0; lastSequence = undefined; received = painted = 0; sampledAt = performance.now();
    if (!native) next.on('data', frame => {
      if (stream !== next) return;
      if (frame.discontinuity) clear();
      else { stage.classList.add('ready');  }
      if (!frame.discontinuity && lastSequence !== undefined && frame.sequence > lastSequence + 1n) sequenceGaps += Number(frame.sequence - lastSequence - 1n);
      lastSequence = frame.sequence;
      ++received; renderer.push(frame);
    });
    next.on('error', error => { if (stream === next) { if (native && error.code === 'VIDEO_RENDERER_UNAVAILABLE') nativeUnavailable = true; clear(); report(error); } });
    next.on('close', () => { if (stream === next) {
      nativeLayout?.dispose(); nativeLayout = null; nativeMetrics = null;
      stream = null; document.documentElement.classList.remove('native-video'); clear();
    } });
    if (next.latest?.()) renderer.push(next.latest());
    status('connected');
  } catch (error) { report(error); if (error.code === 'VIDEO_FRAME_PENDING' && wanted) dirty = true; }
  finally {
    busy = false;
    if (dirty) { dirty = false; schedule(); }
  }
}
function schedule() { clearTimeout(resizeTimer); resizeTimer = setTimeout(() => reconcile().catch(report), 300); }
async function toggleFullscreen() {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else {
      if (!document.fullscreenEnabled || !stage.requestFullscreen) throw Object.assign(new Error(t('fullscreenUnavailable')), { code: 'FULLSCREEN_UNAVAILABLE', messageKey: 'fullscreenUnavailable' });
      await stage.requestFullscreen();
    }
  } catch (error) { report(error); }
  finally { $('fullscreen').checked = !!document.fullscreenElement; }
}
$('fullscreen').onchange = toggleFullscreen;
$('exit-fullscreen').onclick = toggleFullscreen;
stage.ondblclick = event => { if (event.target.closest('button')) return; toggleFullscreen(); };
document.addEventListener('fullscreenchange', () => { $('fullscreen').checked = !!document.fullscreenElement; layout(); schedule(); });
$('aspect').onchange = () => { layout(); schedule(); };
$('resolution').onchange = () => { layout(); schedule(); };
$('apply').onclick = schedule;
$('original').onchange = async () => {
  $('original').disabled = true;
  try {
    if ($('original').checked) {
      const info = await reaper.video.getSourceInfo();
      if (!info.available || !info.originalSupported) throw Object.assign(new Error(t('originalUnavailable')), { code: info.error || 'VIDEO_RESOLUTION_LIMIT', messageKey: 'originalUnavailable' });
      source = info; previousMode = $('resolution').value; $('resolution').value = 'source';
      if (!document.fullscreenElement && !await reaper.window.isDocked()) {
        const [size, position, displays] = await Promise.all([reaper.window.getSize(), reaper.window.getPosition(), reaper.system.getDisplays()]);
        const display = displays.find(d => position.x + size.width / 2 >= d.bounds.x && position.x + size.width / 2 < d.bounds.x + d.bounds.width && position.y + size.height / 2 >= d.bounds.y && position.y + size.height / 2 < d.bounds.y + d.bounds.height) || displays.find(d => d.primary) || displays[0];
        if (display) {
          savedWindow = { ...size, ...position };
          const pixelScale = capabilities.platform === 'macos' ? display.scaleFactor : 1;
          const cssScale = devicePixelRatio / pixelScale, area = display.workArea;
          const width = Math.min(area.width, Math.max(480 * cssScale, info.width / pixelScale + size.width - innerWidth * cssScale));
          const height = Math.min(area.height, Math.max(360 * cssScale, info.height / pixelScale + size.height - stage.clientHeight * cssScale));
          await reaper.window.setSize(Math.round(width), Math.round(height));
          await reaper.window.setPosition(Math.round(Math.max(area.x, Math.min(position.x, area.x + area.width - width))), Math.round(Math.max(area.y, Math.min(position.y, area.y + area.height - height))));
        }
      }
    } else {
      $('resolution').value = previousMode;
      if (savedWindow && !document.fullscreenElement && !await reaper.window.isDocked()) {
        await reaper.window.setSize(savedWindow.width, savedWindow.height);
        await reaper.window.setPosition(savedWindow.x, savedWindow.y);
      }
      savedWindow = null;
    }
    layout(); schedule();
  } catch (error) { $('original').checked = false; layout(); report(error); }
  finally { $('original').disabled = false; }
};
const observer = new ResizeObserver(() => { layout(); if (options().resolution === 'fit') schedule(); }); observer.observe(stage);
window.addEventListener('resize', () => { layout(); schedule(); });
document.addEventListener('scroll', () => { if (nativeLayout) layout(); }, true);
document.addEventListener('visibilitychange', () => reconcile().catch(report));
let metricData;
function renderMetrics() {
  if (!metricData) return;
  const data = metricData, rect = canvas.getBoundingClientRect();
  const native = stream?.readablePixels === false;
  const observed = native && (stream.info?.renderer === 'native-d3d11' || nativeMetrics?.presentationCounter === 'dxgi-observed');
  const rates = `${data.targetUpdateRate.toFixed(2)} / ${data.actualFPS.toFixed(1)}`;
  const values = [[t('source'), source.width ? `${source.width} × ${source.height}` : t('pending')], [t('stream'), `${data.width} × ${data.height}`], [t(native ? 'nativeBox' : 'canvas'), `${Math.round(rect.width * devicePixelRatio)} × ${Math.round(rect.height * devicePixelRatio)} px`], [t(observed ? 'nativeObservedFps' : native ? 'nativeFps' : 'fps'), native ? `${rates} / ${Number.isFinite(drawRate) ? drawRate.toFixed(1) : '—'}` : `${rates} / ${receiveRate.toFixed(1)} / ${drawRate.toFixed(1)}`, observed ? t('nativeObservedHint') : '']];
  $('metrics').replaceChildren(...values.map(([label, value, hint]) => { const span = document.createElement('span'), small = document.createElement('small'); small.textContent = label; if (hint) span.title = hint; span.append(small, document.createTextNode(value)); return span; }));
}
translate();
(async () => {
  await reaper.lifecycle.ready;
  await reaper.window.setIconVisible(false);
  capabilities = await reaper.video.getCapabilities();
  if (!preferNative()) await selectRenderer('rgba8');
  if (!capabilities.available || !capabilities.resolutionModes) throw Object.assign(new Error(t('install')), { code: capabilities.error || 'VIDEO_UNAVAILABLE', messageKey: 'install' });
  status('ready');
  await reaper.lifecycle.on('cleanup', async () => { closing = true; wanted = false; clearInterval(poll); clearTimeout(resizeTimer); observer.disconnect(); await detach(); renderer?.dispose(); });
  let polling = false;
  poll = setInterval(async () => {
    if (polling || closing || document.hidden || !wanted) return;
    polling = true;
    try {
      if (!stream) { await reconcile(); return; }
      const current = stream, native = current.readablePixels === false;
      const data = await reaper.video.getDiagnostics();
      const surface = native ? await current.getDiagnostics() : null;
      if (stream !== current || current.closed) return;
      source = { width: data.sourceWidth, height: data.sourceHeight };
      if (data.status !== 'ready' || (surface && !surface.hasFrame)) clear();
      else if (native) stage.classList.add('ready');
      const seconds = (performance.now() - sampledAt) / 1000;
      receiveRate = received / seconds;
      drawRate = native ? nativePresentationRate(surface, previousPresented, seconds) : painted / seconds;
      if (native) { previousPresented = surface.presentedFrames; nativeMetrics = surface; }
      received = painted = 0; sampledAt = performance.now();
      metricData = data; renderMetrics();
      $('diagnostics').textContent = JSON.stringify(native ? { ...data, ...nativeMetrics, presentationFPS: drawRate, readablePixels: false, devicePixelRatio } :
        { ...data, receivedFPS: receiveRate, canvasDrawFPS: drawRate, transportSequenceGaps: sequenceGaps, rendererDropped: renderer.dropped, renderer: renderer.kind, devicePixelRatio }, null, 2);
      if (data.error) report({ code: data.error, message: data.error });
      else status($('original').checked ? 'originalStatus' : 'live');
    } catch (error) { report(error); }
    finally { polling = false; }
  }, 1000);
  wanted = true;
  await reconcile();
})().catch(report);
