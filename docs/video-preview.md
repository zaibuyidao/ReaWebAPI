# Project video preview

`reaper.video` streams the current project's final video through Native Frame Streams. The preview includes the project's video processing effects.

Run [reawebapi-video-preview.lua](../examples/integrations/video-preview/reawebapi-video-preview.lua) in `examples/integrations/video-preview` for the Canvas demo. The video demo is not included in ReaPack.

## Host compatibility

Install the v0.3.9.32 or newer extension for the host platform and architecture. The video backend checks a normalized module fingerprint rather than the REAPER version string. Selected build metadata fields are excluded from the fingerprint while code, data and layout remain checked to protect calls through fixed internal offsets.

The backend supports Windows x64, macOS arm64 and Linux x86_64. Normalization excludes PE build timestamps, checksums and CodeView identifiers, Mach-O UUIDs and code-signature payloads, and ELF GNU build IDs. Linux selects the offset mapping for the matching module layout, including REAPER 7.82, 7.84 and 7.85.

`getCapabilities()` reports backend startup prerequisites and shared profile limits. The backend is `reaper-internal`. If the module fingerprint does not match, the module cannot be read, or required host APIs or a platform/architecture implementation are missing, `available` is `false`, `openStream()` rejects with `VIDEO_HOST_UNSUPPORTED`, and the demo displays an unavailable message. No desktop-capture permission is required.

Internal calls are allowed only when the normalized module fingerprint matches the fixed offsets. Rebuilds changing only these normalized fields can pass without adding a version entry. Changes to code, data or layout still require compatibility verification. This check does not replace host-level testing.

## API

```js
await reaper.lifecycle.ready;
const capabilities = await reaper.video.getCapabilities();
if (!capabilities.available) throw new Error(capabilities.error);
const stream = await reaper.video.openStream({
  source: 'project', width: 1280, height: 720, updateRate: 'auto'
});
canvas.width = stream.info.width;
canvas.height = stream.info.height;
stream.on('data', frame => {
  context.putImageData(new ImageData(new Uint8ClampedArray(
    frame.bytes.buffer, frame.bytes.byteOffset, frame.bytes.byteLength
  ), stream.info.width, stream.info.height), 0, 0);
});
const clear = () => context.clearRect(0, 0, canvas.width, canvas.height);
stream.on('error', error => { clear(); console.error(error.code, error.message); });
stream.on('close', clear);
await reaper.lifecycle.on('cleanup', () => stream.close());
```

The returned object is an ordinary Native Stream consumer, including `latest()`, `on('data' | 'error' | 'close')` and `close()`. Media-frame timestamps are REAPER project seconds. A WebGL renderer can upload `frame.bytes` as an RGBA8 texture. Coalesce Canvas/WebGL updates through `requestAnimationFrame`.

The demo uses a continuous `requestAnimationFrame` loop and an ordered queue of at most six packets, drawing one per refresh and discarding the oldest packet on overflow. Startup waits for a second packet or about 16 ms, nominally one frame at 60 FPS, so a solitary stopped frame also appears. Project video invalidation packets carry `discontinuity: true` to discard queued pre-change frames, including at the same position. Backward timestamps or forward jumps exceeding elapsed arrival time by over 150 ms reset the queue. Closing or errors cancel queued drawing. Buffering smooths arrival timing but adds latency, and the browser/display refresh rate still limits presentation.

Project video retains up to three captured frames in order. Each consumer can have two frames in flight and two waiting for transmission. A full waiting queue drops its oldest frame, so a slow consumer cannot accumulate unlimited latency or stall other consumers. The transport reuses one capture-copy buffer and up to three outgoing buffers, released when project video closes. Buffers still referenced by a consumer are never overwritten. Other Native Stream buffering policies are unchanged.

`getState()` exposes source dimensions, the latest captured project position, frame age, subscribers and availability. `getDiagnostics()` adds captured/produced frames, `clearedFrames`, capture failures, dropped frames, average capture FPS and capture time. `actualFPS` counts successful captures, not distinct visual content or displayed frames. Static images and lower-frame-rate media can still produce visually identical captures. Clear packets are excluded from `capturedFrames` and included in `producedFrames` when publication succeeds. `transportLatencyMs` measures the latest frame's send-to-ACK round trip from any consumer, including page callback handling but excluding Canvas/GPU presentation. It is `null` until an ACK arrives. Additional transport diagnostics remain in `reaper.stream.getDiagnostics()`.

## Invalidation and state

A new stream first publishes an opaque black clear packet at the configured dimensions. Project edits, seeks and playback-state changes immediately invalidate retained frames. Missing input and REAPER rendering also clear a previously valid image. During transient pending decode, the last valid frame is retained while decoding remains pending for up to 100 ms, measured from the first pending read rather than the last successful capture. A clear packet removes the consumer's retained image and does not indicate an available video source.

`status === 'ready'` identifies a valid source frame, including one retained during transient pending decode. Once cleared, `sourceWidth` and `sourceHeight` are `0` and `position` is `null`. Do not infer availability from packet arrival or black pixels alone. Clear the Canvas when the stream closes or reports an error.

| Error | Behavior |
| --- | --- |
| `VIDEO_FRAME_PENDING` | Wait for a frame consistent with the current project state |
| `VIDEO_SOURCE_UNAVAILABLE` | No usable source frame or an invalid frame layout |
| `VIDEO_RENDER_BUSY` | Pause preview capture while REAPER is rendering, then resume |
| `VIDEO_PROJECT_CHANGED` | Close the old stream and require reopening for the current project |
| `VIDEO_HOST_UNSUPPORTED` | Reject opening when backend startup prerequisites are missing |

## Ownership and limits

- `updateRate: 'auto'` follows the current project frame rate, including fractional rates such as 23.976 and 29.97, and updates while the stream remains open. The target is limited to 1–75 FPS, with a 30 FPS fallback when the project rate is unavailable. Numeric values retain the fixed 1–75 FPS mode, including fractional rates. Omitting the option selects `auto`, as does the Demo.
- State and diagnostics expose the requested `updateRate` and current `targetUpdateRate`. `stream.info.updateRate` is the numeric target at stream creation. Actual delivery depends on source decoding, host load and rendering. Displayed FPS is also limited by the display refresh rate. Auto and fixed modes are different shared profiles even when their current target rates match.
- One shared profile per current project. The first consumer selects width, height and update rate. Subsequent consumers must use the same values, otherwise `VIDEO_PROFILE_CONFLICT` is returned. Defaults are 1280×720 with automatic project frame-rate tracking. Limits are 1920×1080 and 75 FPS.
- Output dimensions are fixed, with aspect-preserving scaling and opaque black letterboxing. Input resolution changes do not alter the stream descriptor. A dedicated video scheduler wakes the REAPER main thread independently of the extension timer. Internal video calls, pixel scaling/copying and publication remain on that thread. Frame transport uses the existing bounded binary stream.
- On Windows, an active project video stream enables high-resolution waits in the transport worker to reduce polling and ACK delays, with ordinary waits as a fallback. This leaves system timer resolution, packet header size and the ACK protocol unchanged.
- During continuous playback, an unseen selected host frame is read before advancing video. It may lag the current query by at most one frame and must still match the project source, cached frame and monitor processing. Unchanged selected frames are identified by query index, source and monitor hashes, and frame timestamps, avoiding repeated conversion and copying. During normal or slower playback, auto reads distinct host frames directly when the project rate is at most 75 FPS. Fixed rates, faster playback and higher project rates retain deadline throttling. Unchanged or pending frames are retried at the next eligible scheduler wake without consuming another target interval. Stopped previews continue to refresh, and invalidation forces a fresh capture. This does not guarantee delivery of every source frame.
- REAPER converts native frames to its RGBA format, which uses BGRA byte order. The extension copies and scales those pixels into web RGBA8. This preserves host YUV conversion without selecting a separate color matrix or decoding media again. Source frame references remain local to capture and are released after copying. Only owned pixel bytes enter the stream.
- Video processing is activated on demand. The last consumer detaching releases the backend's scheduling state. An unclaimed stream expires after ten seconds. Closing, reloading and unloading use the existing stream cleanup.
- Hiding the App/page closes its video consumers, including a Dock transition that hides the page. Reopen when visible again. Opening from a hidden page rejects with `VIDEO_PAGE_HIDDEN`.
- Project changes close the old stream and increment its source epoch. Reopen after the project change. Consult state before presenting retained frames as current.

The backend depends on private REAPER functions and state. Compatibility across versions still requires host-level testing. RGBA and YV12 input paths are validated. YUY2 is delegated to the same host conversion path and has not had separate fixture acceptance. Native output scaling, display color management and GPU presentation can differ from the captured source pixels.

Run `tests/video_reaper.py` against a new disposable profile for host acceptance. Compile/unit tests alone do not establish host-level video compatibility.
