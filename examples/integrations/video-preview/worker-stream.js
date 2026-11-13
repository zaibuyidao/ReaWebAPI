// Video-only worker protocol. Shared slots remain leased until GPU upload returns.
export function connectVideoStream({ port, descriptor, buffer }, { resize, push, clear }) {
  let socket, stopped = false;
  const fail = message => close('UNSUPPORTED_FORMAT', message);
  const close = (code = 'STREAM_CLOSED', message = 'Video stream closed') => {
    if (stopped) return;
    stopped = true; clear(); socket?.close();
    port.postMessage({ type: 'closed', code, message, buffer }, buffer ? [buffer] : []);
    buffer = null; port.close();
  };
  port.onmessage = ({ data }) => { if (data?.type === 'close') close(); };
  port.onmessageerror = () => close('TRANSPORT_ERROR', 'Video control message could not be read');
  try {
    const { width, height, stride, maxBytes, sharedVideo } = descriptor;
    const planar = descriptor.format === 'i420';
    const dynamic = descriptor.dynamicResolution === true && descriptor.layoutVersion === 3;
    const header = dynamic ? 56 : 40;
    if (descriptor.kind !== 'frame' || !['rgba8', 'i420'].includes(descriptor.format) || !Number.isSafeInteger(maxBytes) || maxBytes <= 0 ||
        maxBytes > 40 * 1024 * 1024 || (descriptor.layoutVersion === 3) !== dynamic ||
        !Number.isSafeInteger(width) || !Number.isSafeInteger(height) || width <= 0 || height <= 0 ||
        width > 8192 || height > 8192 || stride !== width * (planar ? 1 : 4) ||
        (dynamic ? maxBytes < stride * height * (planar ? 1.5 : 1) : maxBytes !== stride * height * (planar ? 1.5 : 1)) ||
        (planar && (width % 2 || height % 2 || descriptor.colorMatrix !== 'bt709' || descriptor.colorRange !== 'limited' ||
          !['reaper-neon', 'reaper-sse'].includes(descriptor.colorConversion))) ||
        (sharedVideo && buffer?.byteLength !== maxBytes * 2)) throw new Error('Invalid video descriptor');
    const slots = sharedVideo ? [new Uint8Array(buffer, 0, maxBytes), new Uint8Array(buffer, maxBytes, maxBytes)] : null;
    const leased = new Set();
    resize(width, height, descriptor);
    socket = new WebSocket(descriptor.url); socket.binaryType = 'arraybuffer';
    socket.onopen = () => { if (!stopped) port.postMessage({ type: 'ready' }); };
    socket.onerror = () => close('TRANSPORT_ERROR', 'Video stream connection failed');
    socket.onclose = event => close(event.reason || 'TRANSPORT_ERROR', 'Video stream connection closed');
    socket.onmessage = ({ data }) => {
      if (stopped) return;
      try {
        const view = new DataView(data), shared = !!sharedVideo;
        if (data.byteLength < header || view.getUint32(0, true) !== (dynamic ? 0x03535752 : 0x01535752) || view.getUint8(4) !== 1 ||
            !!(view.getUint8(5) & 2) !== shared) throw new Error('Invalid video packet');
        const size = Number(view.getBigUint64(32, true));
        const w = dynamic ? view.getUint32(40, true) : width, h = dynamic ? view.getUint32(44, true) : height;
        const pitch = dynamic ? view.getUint32(48, true) : stride;
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || pitch !== w * (planar ? 1 : 4) ||
            (dynamic && w * h * 4 > 40 * 1024 * 1024) ||
            (planar && (w % 2 || h % 2)) || size !== pitch * h * (planar ? 1.5 : 1) || size > maxBytes ||
            data.byteLength !== (shared ? header + 4 : header + size)) throw new Error('Invalid video dimensions');
        const slot = shared ? view.getUint32(header, true) : -1;
        if (shared && (slot > 1 || leased.has(slot))) throw new Error('Invalid video slot ownership');
        const sequence = view.getBigUint64(8, true), timestamp = view.getFloat64(16, true), producerDropped = view.getBigUint64(24, true);
        if (!Number.isFinite(timestamp)) throw new Error('Invalid video timestamp');
        const discontinuity = !!(view.getUint8(5) & 1), bytes = shared ? slots[slot].subarray(0, size) : new Uint8Array(data, header);
        let released = false;
        if (shared) leased.add(slot);
        port.postMessage({ type: 'frame', frame: { sequence, timestamp, producerDropped, discontinuity, width: w, height: h, stride: pitch } });
        push({ sequence, timestamp, producerDropped, discontinuity, width: w, height: h, stride: pitch, bytes, release() {
          if (released) return;
          released = true; leased.delete(slot);
          if (!stopped && socket.readyState === WebSocket.OPEN) socket.send(new Uint8Array([1]));
        } });
      } catch (error) { fail(error.message); }
    };
  } catch (error) { fail(error.message); }
  return close;
}
