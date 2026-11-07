# Project video preview

`reaper.video` streams the current project's final video through Native Frame Streams. The preview includes the project's video processing effects.

Run [reawebapi-video-preview.lua](../examples/integrations/video-preview/reawebapi-video-preview.lua) in `examples/integrations/video-preview` for the Canvas demo. The video demo is not included in ReaPack.

## Host compatibility

Install the v0.3.9.28 extension for the host platform and architecture. Video is enabled only for these verified REAPER module builds:

| Platform | REAPER | Architecture |
| --- | --- | --- |
| Windows | 7.84 | x64 |
| macOS | 7.84 | arm64 |
| Linux | 7.82 | x86_64 |

Compatibility is checked against the video module's SHA-256, not just the version string. For other builds and architectures, `getCapabilities().available` is `false` and `openStream()` rejects with `VIDEO_HOST_UNSUPPORTED`. Other extension features remain available. No desktop-capture permission is required.

`getCapabilities()` reports backend compatibility, the `supportedHosts` platform/architecture/version list and profile limits. Custom binaries with a listed version must still match the module fingerprint. The backend is `reaper-internal`.

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

`getState()` exposes source dimensions, the latest captured project position, frame age, subscribers and availability. `getDiagnostics()` adds captured/produced frames, `clearedFrames`, capture failures, dropped frames, average capture FPS and capture time. Clear packets are excluded from `capturedFrames` and included in `producedFrames` when publication succeeds. `transportLatencyMs` measures the latest frame's send-to-ACK round trip from any consumer, including page callback handling but excluding Canvas/GPU presentation. It is `null` until an ACK arrives. Additional transport diagnostics remain in `reaper.stream.getDiagnostics()`.

## Invalidation and state

A new stream first publishes an opaque black clear packet at the configured dimensions. Project edits, seeks and playback-state changes invalidate retained frames. Missing input, pending decode and REAPER rendering also clear a previously valid image. A clear packet removes the consumer's retained image and does not indicate an available video source.

`status === 'ready'` identifies a valid source frame. While waiting, `sourceWidth` and `sourceHeight` are `0` and `position` is `null`. Do not infer availability from packet arrival or black pixels alone. Clear the Canvas when the stream closes or reports an error.

| Error | Behavior |
| --- | --- |
| `VIDEO_FRAME_PENDING` | Wait for a frame consistent with the current project state |
| `VIDEO_SOURCE_UNAVAILABLE` | No usable source frame or an invalid frame layout |
| `VIDEO_RENDER_BUSY` | Pause preview capture while REAPER is rendering, then resume |
| `VIDEO_PROJECT_CHANGED` | Close the old stream and require reopening for the current project |
| `VIDEO_HOST_UNSUPPORTED` | Reject opening without calling unknown internal video functions |

## Ownership and limits

- `updateRate: 'auto'` follows the current project frame rate, including fractional rates such as 23.976 and 29.97, and updates while the stream remains open. The target is limited to 1–60 FPS, with a 30 FPS fallback when the project rate is unavailable. Numeric values retain the fixed 1–60 integer FPS mode. Omitting the option selects `auto`, as does the Demo.
- State and diagnostics expose the requested `updateRate` and current `targetUpdateRate`. `stream.info.updateRate` is the numeric target at stream creation. Actual delivery depends on host and rendering load. Auto and fixed modes are different shared profiles even when their current target rates match.
- One shared profile per current project. The first consumer selects width, height and update rate. Subsequent consumers must use the same values, otherwise `VIDEO_PROFILE_CONFLICT` is returned. Defaults are 1280×720 with automatic project frame-rate tracking. Limits are 1920×1080 and 60 FPS.
- Output dimensions are fixed, with aspect-preserving scaling and opaque black letterboxing. Input resolution changes do not alter the stream descriptor. Internal video calls, pixel scaling/copying and publication run on the REAPER main thread. Frame transport uses the existing bounded binary stream.
- REAPER converts native frames to its RGBA format, which uses BGRA byte order. The extension copies and scales those pixels into web RGBA8. This preserves host YUV conversion without selecting a separate color matrix or decoding media again. Source frame references remain local to capture and are released after copying. Only owned pixel bytes enter the stream.
- Video processing is activated on demand. The last consumer detaching releases the backend's scheduling state. An unclaimed stream expires after ten seconds. Closing, reloading and unloading use the existing stream cleanup.
- Hiding the App/page closes its video consumers, including a Dock transition that hides the page. Reopen when visible again. Opening from a hidden page rejects with `VIDEO_PAGE_HIDDEN`.
- Project changes close the old stream and increment its source epoch. Reopen after the project change. Consult state before presenting retained frames as current.

The backend depends on private REAPER functions and state. Each additional host build requires independent compatibility verification. RGBA and YV12 input paths are validated. YUY2 is delegated to the same host conversion path and has not had separate fixture acceptance. Native output scaling, display color management and GPU presentation can differ from the captured source pixels.

Run `tests/video_reaper.py` against a new disposable profile for host acceptance. Compile/unit tests alone do not establish host-level video compatibility.
