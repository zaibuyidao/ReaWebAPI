import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
const source = await readFile(new URL('../examples/integrations/video-preview/worker-stream.js', import.meta.url), 'utf8');
const { connectVideoStream } = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
let socket;
globalThis.WebSocket = class {
  static OPEN = 1;
  constructor() { socket = this; this.readyState = 1; this.sent = []; }
  send(data) { this.sent.push(data); }
  close() { this.readyState = 3; this.onclose?.({ reason: 'STREAM_CLOSED' }); }
};
const descriptor = { kind: 'frame', format: 'rgba8', width: 2, height: 2, stride: 8, maxBytes: 16, url: 'ws://test' };
function packet(sequence, shared, slot = 0) {
  const data = new ArrayBuffer(shared ? 44 : 56), view = new DataView(data);
  view.setUint32(0, 0x01535752, true); view.setUint8(4, 1); view.setUint8(5, shared ? 2 : 0);
  view.setBigUint64(8, BigInt(sequence), true); view.setFloat64(16, sequence / 60, true); view.setBigUint64(32, 16n, true);
  if (shared) view.setUint32(40, slot, true);
  return { data };
}
for (const shared of [true, false]) {
  let frames = [], messages = [], cleared = 0;
  const buffer = shared ? new ArrayBuffer(32) : null;
  const port = { postMessage: (data, transfer = []) => messages.push(structuredClone(data, { transfer })), close() {} };
  const close = connectVideoStream({ port, descriptor: { ...descriptor, sharedVideo: shared }, buffer }, {
    resize: (w, h) => { assert.equal(w, 2); assert.equal(h, 2); },
    push: frame => frames.push(frame), clear() { for (const frame of frames) frame.release(); frames = []; ++cleared; }
  });
  socket.onopen(); socket.onmessage(packet(1, shared, 0)); socket.onmessage(packet(2, shared, 1));
  assert.equal(socket.sent.length, 0, 'A native slot must not be ACKed while queued for upload');
  if (shared) assert.equal(frames[0].bytes.buffer, buffer, 'No per-frame copy');
  frames[0].release(); frames[0].release(); assert.equal(socket.sent.length, 1);
  socket.onmessage(packet(3, shared, 0));
  assert.equal(messages.filter(m => m.type === 'frame').length, 3);
  close(); close(); assert.equal(cleared, 1);
  assert.equal(messages.at(-1).type, 'closed'); assert.equal(messages.at(-1).buffer?.byteLength || 0, shared ? 32 : 0);
  if (shared) assert.equal(buffer.byteLength, 0, 'Return memory ownership to the host bridge');
  assert.equal(socket.sent.length, 1, 'Closing does not ACK stale slots');
}
for (const bad of ['slot', 'reuse', 'header', 'timestamp']) {
  const messages = [], port = { postMessage: data => messages.push(data), close() {} };
  connectVideoStream({ port, descriptor: { ...descriptor, sharedVideo: true }, buffer: new ArrayBuffer(32) }, { resize() {}, push() {}, clear() {} });
  socket.onmessage(packet(1, true));
  const value = packet(2, true, bad === 'slot' ? 2 : bad === 'reuse' ? 0 : 1), view = new DataView(value.data);
  if (bad === 'header') view.setUint32(0, 0, true);
  if (bad === 'timestamp') view.setFloat64(16, NaN, true);
  socket.onmessage(value);
  assert.equal(messages.at(-1).code, 'UNSUPPORTED_FORMAT'); assert.equal(socket.sent.length, 0);
}
console.log('Video worker shared/raw transport, slot leases, malformed packets and return ownership passed');
for (const conversion of ['reaper-neon', 'reaper-sse', 'unknown']) {
  const messages = [], frames = [], port = { postMessage: data => messages.push(data), close() {} };
  const info = { ...descriptor, format: 'i420', stride: 2, maxBytes: 6, sharedVideo: true,
    colorMatrix: 'bt709', colorRange: 'limited', colorConversion: conversion };
  const close = connectVideoStream({ port, descriptor: info, buffer: new ArrayBuffer(12) }, {
    resize: (w, h, actual) => assert.equal(actual.colorConversion, conversion), push: frame => frames.push(frame), clear() {}
  });
  if (conversion === 'unknown') { assert.equal(messages.at(-1).code, 'UNSUPPORTED_FORMAT'); continue; }
  const value = packet(1, true); new DataView(value.data).setBigUint64(32, 6n, true);
  socket.onmessage(value); assert.equal(frames[0].bytes.byteLength, 6); assert.equal(socket.sent.length, 0);
  frames[0].release(); assert.equal(socket.sent.length, 1); close();
}
