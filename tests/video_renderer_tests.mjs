import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const source = await readFile(new URL('../examples/integrations/video-preview/frame-renderer.js', import.meta.url), 'utf8');
const { createFrameRenderer } = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
let now = 0, next = 1;
const callbacks = new Map();
globalThis.performance = { now: () => now };
globalThis.requestAnimationFrame = callback => { const id = next++; callbacks.set(id, callback); return id; };
globalThis.cancelAnimationFrame = id => callbacks.delete(id);
function wake(at) {
  now = at;
  const pending = [...callbacks.values()];
  callbacks.clear();
  for (const callback of pending) callback(now);
}
const frame = (id, timestamp = id / 60) => ({ timestamp, bytes: new Uint8Array([id]) });

{
  const drawn = [], frames = Array.from({ length: 7 }, (_, i) => frame(i + 1));
  const renderer = createFrameRenderer(value => drawn.push(value));
  for (const value of frames) renderer.push(value);
  assert.equal(callbacks.size, 1);
  wake(8); wake(16); wake(24); wake(32); wake(40); wake(48);
  assert.deepEqual(drawn, frames.slice(1), 'Overload drops only the oldest packet and preserves FIFO order');
  assert.equal(renderer.dropped, 1);
  assert.deepEqual(frames.map(value => [...value.bytes]), [[1], [2], [3], [4], [5], [6], [7]], 'Incoming pixel buffers remain unchanged');
  assert.equal(callbacks.size, 1, 'RAF continues while the renderer is active');
  renderer.clear();
  assert.equal(callbacks.size, 0);
}
{
  const drawn = [], renderer = createFrameRenderer(value => drawn.push(value));
  renderer.push(frame(1));
  wake(now + 8); assert.equal(drawn.length, 0);
  wake(now + 8); assert.equal(drawn.length, 1, 'A solitary paused frame renders after the startup wait');
  renderer.clear();
  renderer.push(frame(2)); renderer.push(frame(3));
  wake(now + 1); assert.equal(drawn.length, 2, 'A second packet completes startup without waiting 16 ms');
  renderer.clear();
}
{
  const drawn = [], renderer = createFrameRenderer(value => drawn.push(value));
  renderer.push(frame(1)); renderer.push(frame(2));
  now += 50;
  for (let id = 3; id <= 6; ++id) renderer.push(frame(id));
  for (let i = 0; i < 6; ++i) wake(now + 16);
  assert.deepEqual(drawn.map(value => value.bytes[0]), [1, 2, 3, 4, 5, 6], 'Two queued frames plus four transport arrivals fit without loss');
  assert.equal(renderer.dropped, 0);
  renderer.clear();
}
{
  const drawn = [], renderer = createFrameRenderer(value => drawn.push(value));
  renderer.push(frame(1, 1)); renderer.push(frame(2, 1));
  renderer.push({ ...frame(3, 1), discontinuity: true });
  renderer.push(frame(4, 1));
  wake(now + 16); wake(now + 16);
  assert.deepEqual(drawn.map(value => value.bytes[0]), [3, 4], 'Explicit invalidation clears stale frames even at the same position');
  assert.equal(renderer.dropped, 0);
  renderer.clear();
}
for (const timestamp of [0, 2]) {
  const drawn = [], renderer = createFrameRenderer(value => drawn.push(value));
  renderer.push(frame(1, 1)); renderer.push(frame(2, 1.01));
  renderer.push(frame(3, timestamp));
  wake(now + 16);
  assert.deepEqual(drawn.map(value => value.bytes[0]), [3], 'A timestamp discontinuity discards queued pre-seek frames');
  renderer.clear();
}
{
  const drawn = [], renderer = createFrameRenderer(value => drawn.push(value));
  renderer.push(frame(1, 1)); renderer.push(frame(2, 1.01));
  now += 200;
  renderer.push(frame(3, 1.21));
  wake(now + 16); wake(now + 16); wake(now + 16);
  assert.deepEqual(drawn.map(value => value.bytes[0]), [1, 2, 3], 'A playback delay must not discard queued frames as a seek');
  renderer.clear();
}
{
  const drawn = [], renderer = createFrameRenderer(value => drawn.push(value));
  renderer.push(frame(1)); renderer.clear(); renderer.clear();
  wake(now + 20);
  assert.deepEqual(drawn, [], 'Cleanup cancels queued work');
  renderer.push(frame(2)); wake(now + 16);
  assert.equal(drawn.length, 1, 'New data can restart after cleanup');
  renderer.clear();
}
{
  const renderer = createFrameRenderer(() => renderer.clear());
  renderer.push(frame(1)); wake(now + 16);
  assert.equal(callbacks.size, 0, 'Cleanup during drawing cancels the next RAF');
}
console.log('Video renderer queue, startup, discontinuity and cleanup tests passed');
