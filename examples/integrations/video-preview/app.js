'use strict';
const canvas = document.querySelector('canvas'), context = canvas.getContext('2d');
const status = document.querySelector('#status'), diagnostics = document.querySelector('#diagnostics');
const openButton = document.querySelector('#open'), closeButton = document.querySelector('#close');
let stream, opening = false, pendingFrame, painting = 0, poll;
const report = error => { status.textContent = `${error.code || 'VIDEO_ERROR'}: ${error.message}`; };
function clearPreview() {
  pendingFrame = null;
  if (painting) cancelAnimationFrame(painting);
  painting = 0;
  context.clearRect(0, 0, canvas.width, canvas.height);
}
async function closeVideo() {
  clearPreview();
  const old = stream; stream = null;
  closeButton.disabled = true; openButton.disabled = false;
  await old?.close();
}
async function openVideo() {
  if (stream || opening || document.hidden) return;
  opening = true; openButton.disabled = true;
  try {
    const next = await reaper.video.openStream({ source: 'project', width: 1280, height: 720, updateRate: 'auto' });
    if (document.hidden) { await next.close(); return; }
    stream = next; closeButton.disabled = false;
    canvas.width = next.info.width; canvas.height = next.info.height;
    const draw = frame => {
      pendingFrame = frame;
      if (painting) return;
      painting = requestAnimationFrame(() => {
        painting = 0;
        if (!pendingFrame || stream !== next) return;
        context.putImageData(new ImageData(new Uint8ClampedArray(pendingFrame.bytes.buffer,
          pendingFrame.bytes.byteOffset, pendingFrame.bytes.byteLength), canvas.width, canvas.height), 0, 0);
        pendingFrame = null;
      });
    };
    next.on('data', draw);
    if (next.latest()) draw(next.latest());
    next.on('error', error => { if (stream === next) clearPreview(); report(error); });
    next.on('close', error => {
      if (stream === next) { closeVideo().catch(report); report(error); }
    });
  } catch (error) { report(error); }
  finally { opening = false; openButton.disabled = !!stream; }
}
openButton.onclick = openVideo;
closeButton.onclick = () => closeVideo().catch(report);
document.addEventListener('visibilitychange', () => { if (document.hidden) closeVideo().catch(report); });
(async () => {
  await reaper.lifecycle.ready;
  const caps = await reaper.video.getCapabilities();
  if (!caps.available) {
    openButton.disabled = true;
    throw Object.assign(new Error(caps.error === 'VIDEO_HOST_UNSUPPORTED'
      ? 'Video preview is unavailable on this host.' : 'The REAPER video backend is unavailable.'), { code: caps.error });
  }
  status.textContent = 'Open a stream to preview the current project video.';
  await reaper.lifecycle.on('cleanup', async () => { clearInterval(poll); await closeVideo(); });
  poll = setInterval(async () => {
    if (!stream) return;
    try {
      const data = await reaper.video.getDiagnostics();
      diagnostics.textContent = JSON.stringify(data, null, 2);
      status.textContent = data.error || `${data.status} · ${data.sourceWidth}×${data.sourceHeight} · Auto target ${data.targetUpdateRate.toFixed(3)} FPS · Actual ${data.actualFPS.toFixed(1)} FPS`;
      if (data.status !== 'ready') clearPreview();
    } catch (error) { report(error); }
  }, 1000);
})().catch(report);
