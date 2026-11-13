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
for (const shared of [false, true]) for (const format of ['rgba8', 'i420']) {
  const planar = format === 'i420', maxBytes = 64, frames = [], messages = [], resizes = [];
  const buffer = shared ? new ArrayBuffer(maxBytes * 2) : null;
  const info = { ...descriptor, format, stride: planar ? 2 : 8, maxBytes, sharedVideo: shared,
    dynamicResolution: true, layoutVersion: 3, colorMatrix: 'bt709', colorRange: 'limited', colorConversion: 'reaper-sse' };
  const close = connectVideoStream({ descriptor: info, buffer, port: { postMessage: value => messages.push(value), close() {} } }, {
    resize: (w, h) => resizes.push([w, h]), push: frame => frames.push(frame), clear() {}
  });
  socket.onopen();
  for (const [index, [width, height]] of [[2, 2], [4, 2], [2, 4], [2, 2]].entries()) {
    const size = width * height * (planar ? 1.5 : 4), slot = index % 2;
    const data = new ArrayBuffer(shared ? 60 : 56 + size), view = new DataView(data);
    view.setUint32(0, 0x03535752, true); view.setUint8(4, 1); view.setUint8(5, shared ? 2 : 0);
    view.setBigUint64(8, BigInt(index), true); view.setFloat64(16, index / 60, true); view.setBigUint64(32, BigInt(size), true);
    view.setUint32(40, width, true); view.setUint32(44, height, true); view.setUint32(48, width * (planar ? 1 : 4), true);
    if (shared) { view.setUint32(56, slot, true); new Uint8Array(buffer, slot * maxBytes, size).fill(index); }
    else new Uint8Array(data, 56).fill(index);
    socket.onmessage({ data });
    const frame = frames.at(-1);
    assert.deepEqual([frame.width, frame.height, frame.bytes.byteLength], [width, height, size]);
    assert.ok(frame.bytes.every(value => value === index));
    assert.equal(socket.sent.length, index, 'The current frame keeps its slot until upload');
    frame.release();
  }
  assert.deepEqual(resizes, [[2, 2]], 'Incoming sizes must not clear queued frames or the visible surface');
  assert.equal(messages.filter(value => value.type === 'frame').length, 4);
  assert.equal(socket.sent.length, 4); close();
}
for (const invalid of ['width', 'stride', 'size', 'odd-i420', 'budget']) {
  const messages = [];
  const planar = invalid === 'odd-i420';
  connectVideoStream({ descriptor: { ...descriptor, format: planar ? 'i420' : 'rgba8', stride: planar ? 2 : 8,
    dynamicResolution: true, layoutVersion: 3, maxBytes: 64, sharedVideo: true,
    colorMatrix: 'bt709', colorRange: 'limited', colorConversion: 'reaper-sse' }, buffer: new ArrayBuffer(128),
    port: { postMessage: value => messages.push(value), close() {} } }, { resize() {}, push() {}, clear() {} });
  const data = new ArrayBuffer(60), view = new DataView(data);
  view.setUint32(0, 0x03535752, true); view.setUint8(4, 1); view.setUint8(5, 2);
  view.setBigUint64(32, invalid === 'size' ? 15n : invalid === 'budget' ? 256n : planar ? 6n : 16n, true);
  view.setUint32(40, invalid === 'width' ? 0 : planar ? 1 : invalid === 'budget' ? 8 : 2, true);
  view.setUint32(44, invalid === 'budget' ? 8 : 2, true);
  view.setUint32(48, invalid === 'stride' ? 7 : planar ? 1 : invalid === 'budget' ? 32 : 8, true);
  socket.onmessage({ data });
  assert.equal(messages.at(-1).code, 'UNSUPPORTED_FORMAT'); assert.equal(socket.sent.length, 0);
}
console.log('Dynamic RGBA/I420 shared and raw frames preserve sizes, slot leases and limits');
{
  const messages = [], width = 4096, height = 3072, size = width * height * 1.5;
  connectVideoStream({ descriptor: { ...descriptor, format: 'i420', stride: 2, maxBytes: 40 * 1024 * 1024,
    dynamicResolution: true, layoutVersion: 3, colorMatrix: 'bt709', colorRange: 'limited', colorConversion: 'reaper-sse' },
    port: { postMessage: value => messages.push(value), close() {} } }, { resize() {}, push() {}, clear() {} });
  const data = new ArrayBuffer(56 + size), view = new DataView(data);
  view.setUint32(0, 0x03535752, true); view.setUint8(4, 1); view.setBigUint64(32, BigInt(size), true);
  view.setUint32(40, width, true); view.setUint32(44, height, true); view.setUint32(48, width, true);
  socket.onmessage({ data });
  assert.equal(messages.at(-1).code, 'UNSUPPORTED_FORMAT', 'I420 obeys the RGBA-equivalent pixel budget even when its payload fits');
  assert.equal(socket.sent.length, 0);
}
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
