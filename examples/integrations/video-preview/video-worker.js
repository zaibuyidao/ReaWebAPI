import { createCanvasRenderer } from './canvas-renderer.js';
import { createFrameRenderer } from './frame-renderer.js';
import { connectVideoStream } from './worker-stream.js';
import { createI420Renderer } from './i420-renderer.js';

let canvas, painter, renderer, generation = 0;
self.onmessage = ({ data }) => {
  try {
    if (data.type === 'probe') {
      const probe = new OffscreenCanvas(1, 1);
      const gl = probe.getContext(data.format === 'i420' ? 'webgl2' : 'webgl');
      const supported = !!gl && typeof requestAnimationFrame === 'function';
      gl?.getExtension('WEBGL_lose_context')?.loseContext();
      self.postMessage({ type: 'probe', supported });
    } else if (data.type === 'init') {
      canvas = data.canvas;
      painter = data.format === 'i420' ? createI420Renderer(canvas) : createCanvasRenderer(canvas);
      renderer = createFrameRenderer(frame => {
        if (painter.draw(frame)) self.postMessage({ type: 'draw', generation, sequence: frame.sequence, dropped: renderer.dropped });
      }, frame => frame.release?.());
      self.postMessage({ type: 'ready', kind: painter.kind });
    } else if (data.type === 'reaweb-video') {
      connectVideoStream(data, {
        resize(width, height, info) { renderer.clear(); painter.configure?.(info); canvas.width = width; canvas.height = height; },
        push: frame => renderer.push(frame),
        clear: () => renderer.clear()
      });
    } else if (data.type === 'resize') {
      renderer.clear();
      painter.configure?.(data.info);
      canvas.width = data.width; canvas.height = data.height;
    } else if (data.type === 'present') {
      painter.present?.(data.width, data.height, data.preserveAspect);
    } else if (data.type === 'clear') {
      generation = data.generation; renderer.clear(); painter.clear();
    } else if (data.type === 'frame') {
      renderer.push(data.frame);
      self.postMessage({ type: 'accepted', generation });
    }
  } catch (error) {
    self.postMessage({ type: 'error', code: error.code || 'VIDEO_RENDER_ERROR', message: error.message });
  }
};
