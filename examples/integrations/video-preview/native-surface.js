export function surfaceLayout(canvas, stage, preserveAspect, viewport = { width: innerWidth, height: innerHeight }) {
  const rect = canvas.getBoundingClientRect(), bounds = stage.getBoundingClientRect();
  const x = Math.max(0, bounds.left + stage.clientLeft), y = Math.max(0, bounds.top + stage.clientTop);
  const right = Math.min(viewport.width, bounds.left + stage.clientLeft + stage.clientWidth);
  const bottom = Math.min(viewport.height, bounds.top + stage.clientTop + stage.clientHeight);
  return { rect: { x: rect.left, y: rect.top, width: rect.width, height: rect.height },
    clip: { x: Math.min(viewport.width, x), y: Math.min(viewport.height, y), width: Math.max(0, right - x), height: Math.max(0, bottom - y) }, preserveAspect };
}

export function createSurfaceLayout(surface, canvas, stage, onError) {
  let active = true, request = 0, pending = null, running = false, previous = '';
  const drain = async () => {
    if (running) return;
    running = true;
    try {
      while (active && pending && !surface.closed) {
        const layout = pending; pending = null;
        const key = JSON.stringify(layout);
        if (key === previous) continue;
        await surface.setLayout(layout); previous = key;
      }
    } catch (error) { if (active && !surface.closed) onError(error); }
    finally { running = false; }
  };
  return {
    update(preserveAspect) {
      if (!active) return;
      pending = surfaceLayout(canvas, stage, preserveAspect);
      if (!request) request = requestAnimationFrame(() => { request = 0; void drain(); });
    },
    dispose() { active = false; pending = null; cancelAnimationFrame(request); request = 0; }
  };
}
export function nativePresentationRate(diagnostics, previousFrames, seconds) {
  if (diagnostics.presentationAvailable === false || diagnostics.presentationFPS === null) return null;
  if (Number.isFinite(diagnostics.presentedFrames) && Number.isFinite(previousFrames) && seconds > 0)
    return Math.max(0, diagnostics.presentedFrames - previousFrames) / seconds;
  return Number.isFinite(diagnostics.presentationFPS) ? diagnostics.presentationFPS : null;
}

