export function createFrameRenderer(draw, release = () => {}) {
  let queue = [], request = 0, started = false, firstArrival = 0, lastTimestamp, lastArrival = 0, dropped = 0;
  const reset = () => { for (const frame of queue) release(frame); queue = []; started = false; firstArrival = 0; lastTimestamp = undefined; lastArrival = 0; };
  const tick = now => {
    request = requestAnimationFrame(tick);
    if (!started && queue.length && (queue.length >= 2 || now - firstArrival >= 16)) started = true;
    if (started && queue.length) {
      const frame = queue.shift();
      try { draw(frame); } finally { release(frame); }
    }
  };
  return {
    get dropped() { return dropped; },
    push(frame) {
      const timestamp = frame.timestamp;
      const arrival = performance.now();
      if (frame.discontinuity || (Number.isFinite(timestamp) && Number.isFinite(lastTimestamp) &&
          (timestamp < lastTimestamp || timestamp - lastTimestamp - (arrival - lastArrival) / 1000 > 0.15))) reset();
      if (!queue.length && !started) firstArrival = arrival;
      if (timestamp !== lastTimestamp) lastArrival = arrival;
      lastTimestamp = timestamp;
      queue.push(frame);
      const capacity = Math.max(2, Math.min(6, Math.floor(128 * 1024 * 1024 / Math.max(1, frame.bytes.byteLength))));
      while (queue.length > capacity) { release(queue.shift()); ++dropped; }
      if (!request) request = requestAnimationFrame(tick);
    },
    clear() {
      if (request) cancelAnimationFrame(request);
      request = 0;
      dropped = 0;
      reset();
    }
  };
}
