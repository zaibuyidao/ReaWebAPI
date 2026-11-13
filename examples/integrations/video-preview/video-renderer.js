import { createCanvasRenderer } from './canvas-renderer.js';
import { createFrameRenderer } from './frame-renderer.js';
import { createI420Renderer } from './i420-renderer.js';

export async function createVideoRenderer(canvas, { platform, format = 'rgba8', onDraw = () => {}, onError = () => {} } = {}) {
  if (format === 'i420' && platform !== 'windows') {
    const painter = createI420Renderer(canvas);
    const renderer = createFrameRenderer(frame => {
      try { if (painter.draw(frame)) onDraw(frame.sequence); } catch (error) { onError(error); }
    });
    return {
      kind: painter.kind,
      get dropped() { return renderer.dropped; },
      push: frame => renderer.push(frame),
      resize(width, height, info) { renderer.clear(); painter.configure({ ...info, width, height }); canvas.width = width; canvas.height = height; },
      present: (width, height, preserveAspect) => painter.present(width, height, preserveAspect),
      clear() { renderer.clear(); painter.clear(); },
      dispose() { renderer.clear(); painter.dispose(); }
    };
  }
  const bitmap = platform === 'macos' && typeof createImageBitmap === 'function' ? canvas.getContext('bitmaprenderer') : null;
  if (bitmap) {
    let epoch = 0, pending = 0, dropped = 0, lastSequence;
    const renderer = createFrameRenderer(async frame => {
      if (pending >= 2) { ++dropped; return; }
      const generation = epoch; ++pending;
      try {
        const width = frame.width || canvas.width, height = frame.height || canvas.height;
        const pixels = new ImageData(new Uint8ClampedArray(frame.bytes.buffer, frame.bytes.byteOffset, frame.bytes.byteLength), width, height);
        const image = await createImageBitmap(pixels, { premultiplyAlpha: 'none', colorSpaceConversion: 'none' });
        if (generation !== epoch) { image.close(); return; }
        if (lastSequence !== undefined && frame.sequence <= lastSequence) { image.close(); ++dropped; return; }
        if (canvas.width !== width) canvas.width = width;
        if (canvas.height !== height) canvas.height = height;
        bitmap.transferFromImageBitmap(image); lastSequence = frame.sequence; onDraw(frame.sequence);
      } catch (error) { if (generation === epoch) onError(error); }
      finally { --pending; }
    });
    const clear = () => { ++epoch; lastSequence = undefined; dropped = 0; renderer.clear(); bitmap.transferFromImageBitmap(null); };
    return {
      kind: 'bitmaprenderer',
      get dropped() { return dropped + renderer.dropped; },
      push: frame => renderer.push(frame),
      resize(width, height) { clear(); canvas.width = width; canvas.height = height; },
      clear,
      dispose: clear
    };
  }
  let worker;
  if (typeof Worker === 'function' && canvas.transferControlToOffscreen) {
    try {
      worker = new Worker(new URL('./video-worker.js', import.meta.url), { type: 'module' });
      const supported = await new Promise(resolve => {
        const timeout = setTimeout(() => resolve(false), 3000);
        worker.onmessage = ({ data }) => { clearTimeout(timeout); resolve(data.type === 'probe' && data.supported); };
        worker.onerror = () => { clearTimeout(timeout); resolve(false); };
        worker.postMessage({ type: 'probe', format });
      });
      if (!supported) { worker.terminate(); worker = null; }
    } catch { worker?.terminate(); worker = null; }
  }
  if (!worker) {
    const painter = format === 'i420' ? createI420Renderer(canvas, { matchWindowsPreview: platform === 'windows' }) : createCanvasRenderer(canvas);
    const renderer = createFrameRenderer(frame => {
      try { if (painter.draw(frame)) onDraw(frame.sequence); } catch (error) { onError(error); }
    });
    return {
      kind: painter.kind,
      get dropped() { return renderer.dropped; },
      push: frame => renderer.push(frame),
      resize(width, height, info) { renderer.clear(); painter.configure?.(info); canvas.width = width; canvas.height = height; },
      present: painter.present ? (width, height, preserveAspect) => painter.present(width, height, preserveAspect) : undefined,
      clear() { renderer.clear(); painter.clear(); },
      dispose() { renderer.clear(); painter.dispose(); }
    };
  }
  let generation = 0, pending = 0, dropped = 0, workerDropped = 0, disposed = false;
  const kind = await new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error('Video worker initialization timed out')), 3000);
    worker.onmessage = ({ data }) => {
      if (data.type === 'ready') { clearTimeout(timeout); resolve(data.kind); }
      else if (data.type === 'error') { clearTimeout(timeout); reject(Object.assign(new Error(data.message), { code: data.code })); }
    };
    worker.onerror = event => { clearTimeout(timeout); reject(new Error(event.message || 'Video worker initialization failed')); };
    const offscreen = canvas.transferControlToOffscreen();
    worker.postMessage({ type: 'init', canvas: offscreen, format, matchWindowsPreview: platform === 'windows' && format === 'i420' }, [offscreen]);
  }).catch(error => { worker.terminate(); throw error; });
  worker.onerror = event => { if (!disposed) onError(new Error(event.message || 'Video worker failed')); };
  worker.onmessage = ({ data }) => {
    if (disposed) return;
    if (data.type === 'error') { onError(Object.assign(new Error(data.message), { code: data.code })); return; }
    if (data.generation !== generation) return;
    if (data.type === 'accepted') pending = Math.max(0, pending - 1);
    if (data.type === 'draw') { workerDropped = data.dropped; onDraw(data.sequence); }
  };
  return {
    kind: `worker-${kind}`,
    openStream: platform === 'windows' && globalThis.reaper?.video.openWorkerStream
      ? options => reaper.video.openWorkerStream(worker, options) : undefined,
    get dropped() { return dropped + workerDropped; },
    push(frame) {
      if (!frame.bytes) return;
      if (disposed || !frame.bytes.byteLength) return;
      if (pending >= 6) { ++dropped; return; }
      ++pending;
      // The demo gives ownership to its renderer after reading frame metadata.
      worker.postMessage({ type: 'frame', frame: { bytes: frame.bytes, timestamp: frame.timestamp,
        sequence: frame.sequence, discontinuity: frame.discontinuity, width: frame.width, height: frame.height, stride: frame.stride } }, [frame.bytes.buffer]);
    },
    resize(width, height, info) { worker.postMessage({ type: 'resize', width, height, info }); },
    present: format === 'i420' ? (width, height, preserveAspect) => {
      if (!disposed) worker.postMessage({ type: 'present', width, height, preserveAspect });
    } : undefined,
    clear() {
      if (disposed) return;
      ++generation; pending = dropped = workerDropped = 0;
      worker.postMessage({ type: 'clear', generation });
    },
    dispose() { disposed = true; worker.terminate(); }
  };
}
