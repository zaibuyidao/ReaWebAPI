export function createFrameRenderer(draw) {
  let queue = [], request = 0, started = false, firstArrival = 0, lastTimestamp, lastArrival = 0, dropped = 0;
  const reset = () => { queue = []; started = false; firstArrival = 0; lastTimestamp = undefined; lastArrival = 0; };
  const tick = now => {
    request = requestAnimationFrame(tick);
    if (!started && queue.length && (queue.length >= 2 || now - firstArrival >= 16)) started = true;
    if (started && queue.length) draw(queue.shift());
  };
  return {
    get dropped() { return dropped; },
    push(frame) {
      const timestamp = frame.timestamp;
      const arrival = performance.now();
      if (frame.discontinuity || (Number.isFinite(timestamp) && Number.isFinite(lastTimestamp) &&
          (timestamp < lastTimestamp || timestamp - lastTimestamp - (arrival - lastArrival) / 1000 > 0.15))) reset();
      if (!queue.length && !started) firstArrival = arrival;
      lastTimestamp = timestamp; lastArrival = arrival;
      queue.push(frame);
      if (queue.length > 6) { queue.shift(); ++dropped; }
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
