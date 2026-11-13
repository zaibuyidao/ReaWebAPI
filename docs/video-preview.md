# Project video preview

`reaper.video` provides the current project's final video through Native Frame Streams or native presentation surfaces on Windows/macOS. The preview includes the project's video processing effects.

Run [reawebapi-video-preview.lua](../examples/integrations/video-preview/reawebapi-video-preview.lua) in `examples/integrations/video-preview` for the video demo. The video demo is not included in ReaPack.

## Host compatibility

Install the v0.3.9.40 or newer extension for the host platform and architecture. The video backend checks a normalized module fingerprint rather than the REAPER version string. Selected build metadata fields are excluded from the fingerprint while code, data and layout remain checked to protect calls through fixed internal offsets.

The backend supports Windows x64, macOS arm64 and Linux x86_64. Normalization excludes PE build timestamps, checksums and CodeView identifiers, Mach-O UUIDs and code-signature payloads, and ELF GNU build IDs. Linux selects the offset mapping for the matching module layout, including REAPER 7.82, 7.84 and 7.85.

`getCapabilities()` reports backend startup prerequisites and shared profile limits. The backend is `reaper-internal`. If the module fingerprint does not match, the module cannot be read, or required host APIs or a platform/architecture implementation are missing, `available` is `false`, `openStream()` rejects with `VIDEO_HOST_UNSUPPORTED`, and the demo displays an unavailable message. No desktop-capture permission is required.

Internal calls are allowed only when the normalized module fingerprint matches the fixed offsets. Rebuilds changing only these normalized fields can pass without adding a version entry. Changes to code, data or layout still require compatibility verification. This check does not replace host-level testing.

## API

```js
await reaper.lifecycle.ready;
const videoArea = document.querySelector('#stage');
const canvas = videoArea.querySelector('canvas');
const context = canvas.getContext('2d');
const capabilities = await reaper.video.getCapabilities();
if (!capabilities.available) throw new Error(capabilities.error);
const stream = await reaper.video.openStream({
  source: 'project', resolution: 'fit', preserveAspect: true,
  width: Math.round(videoArea.clientWidth * devicePixelRatio),
  height: Math.round(videoArea.clientHeight * devicePixelRatio), updateRate: 'auto'
});
canvas.width = stream.info.width;
canvas.height = stream.info.height;
stream.on('data', frame => {
  if (canvas.width !== frame.width || canvas.height !== frame.height) {
    canvas.width = frame.width;
    canvas.height = frame.height;
  }
  context.putImageData(new ImageData(new Uint8ClampedArray(
    frame.bytes.buffer, frame.bytes.byteOffset, frame.bytes.byteLength
  ), frame.width, frame.height), 0, 0);
});
const clear = () => context.clearRect(0, 0, canvas.width, canvas.height);
stream.on('error', error => { clear(); console.error(error.code, error.message); });
stream.on('close', clear);
await reaper.lifecycle.on('cleanup', () => stream.close());
```

`getSourceInfo()` returns the actual final frame's `width`, `height`, `available`, `originalSupported` and `error`, plus nullable `nativeFormat`, `colorMatrix`, `colorRange` and `colorConversion`. It does not infer dimensions from the media filename or the project frame rate. Pending or absent frames report zero dimensions. Without a stream, pending discovery retains the video backend for at most five seconds to allow asynchronous decoding, then releases it.

`resolution` selects how output dimensions are resolved:

| Mode | Dimensions |
| --- | --- |
| `fit` | Requires physical viewport `width` and `height`. Fits the source inside this box without upscaling. Supply the actual video area's CSS size multiplied by device pixel ratio. |
| `source` | Uses the final frame's exact dimensions. Do not supply `width` or `height`. Unsupported sizes fail explicitly. |
| `custom` | Uses the supplied dimensions. Existing calls remain compatible, including the legacy 1280×720 defaults when omitted. |

`preserveAspect` defaults to `true`. Custom output uses opaque black letterboxing. Setting it to `false` allows stretching to the output box. The same-size conversion preserves RGB pixel values, while resized output uses bilinear filtering. Original resolution does not imply HDR, higher bit depth or matching display color management: the existing output remains RGBA8.

`fit` and `source` require a valid decoded frame. Retry `VIDEO_FRAME_PENDING` after a short delay instead of substituting a guessed source size. Source-size changes update fit/source dimensions within the existing stream. Fit mode recalculates output inside the originally requested viewport bounds without upscaling. Changing the requested viewport or resolution mode still requires reopening. Custom streams retain their specified dimensions. Consumers share the resolution mode, requested viewport or custom dimensions, aspect policy and update rate. A conflicting request leaves other consumers intact.

Fit/source descriptors expose `dynamicResolution: true` and `layoutVersion: 3`. `info.width`, `info.height` and `info.stride` are attachment-time snapshots. Read `frame.width`, `frame.height` and `frame.stride` for each packet, and use `frame.bytes.byteLength` for its payload length. `info.maxBytes` is the 40 MiB capacity limit, not the current frame length. Custom streams retain fixed dimensions and layout version 1.

The returned object is an ordinary Native Stream consumer, including `latest()`, `on('data' | 'error' | 'close')` and `close()`. Media-frame timestamps are REAPER project seconds. A WebGL renderer can upload `frame.bytes` as an RGBA8 texture. Coalesce Canvas/WebGL updates through `requestAnimationFrame`.

The demo's web renderer uses a continuous `requestAnimationFrame` loop and an ordered queue of at most six packets and 128 MiB, drawing one per refresh and discarding the oldest packet on overflow. Startup waits for a second packet or about 16 ms, nominally one frame at 60 FPS, so a solitary stopped frame also appears. Project video invalidation packets carry `discontinuity: true` to discard queued pre-change frames, including at the same position. Backward timestamps or forward jumps exceeding elapsed time since the last distinct host position by over 150 ms reset the queue. Repeated positions followed by normal clock catch-up do not imply a seek. Closing or errors cancel queued drawing. Buffering smooths arrival timing but adds latency, and the browser/display refresh rate still limits presentation.

Project video retains up to three captured frames in order. Raw socket consumers can have three frames in flight, while shared-memory consumers use two. Both paths allow two frames waiting for transmission. A full waiting queue drops its oldest frame, so a slow consumer cannot accumulate unlimited latency or stall other consumers. Project video exchanges owned capture buffers and reuses outgoing payload storage within a 240 MiB capacity budget. Partial socket frames retain a publication reserve for the active profile, so slow socket consumers cannot exhaust storage needed by native-memory consumers. Switching to larger frames can wait for older in-flight frames to release their capacity. Under pressure, replaceable queued snapshots are discarded before admitting a new frame. Buffers still referenced by a consumer are never overwritten. Other Native Stream buffering policies are unchanged.

On supported Windows WebView2 runtimes, project video uses two read-only shared-memory slots per consumer. macOS delivers the same raw RGBA pixels through window-owned local resource URLs without an additional host-side copy. Linux uses a matching WebKit video module to map read-only native slots and copy each frame once into browser-owned memory. Keep `UserPlugins/reawebapi-video-<arch>/reawebapi-video-<arch>.so` alongside the matching helper. Socket metadata and ACKs protect both slots. The SDK completes each pixel copy before acknowledging it, preserving retained packet contents. Dynamic streams reserve two 40 MiB native slots per consumer, keeping their addresses stable while copying only each frame's valid pixels. Native slots have a separate 256 MiB total budget. Unsupported runtimes or exhausted budgets retain raw binary socket transport. Other streams keep their existing transport.

`getState()` exposes source dimensions, the latest captured project position, frame age, subscribers and availability. `getDiagnostics()` adds captured/produced frames, `clearedFrames`, capture failures, dropped frames, average capture FPS and `captureMs`, the most recent successful frame capture time. Empty polling does not overwrite that timing. `actualFPS` counts successful captures, not distinct visual content or displayed frames. Static images and lower-frame-rate media can still produce visually identical captures. Clear packets are excluded from `capturedFrames` and included in `producedFrames` when publication succeeds. `transportLatencyMs` measures the latest frame's send-to-ACK round trip from any consumer, including page callback handling but excluding Canvas/GPU presentation. It is `null` until an ACK arrives. Additional transport diagnostics remain in `reaper.stream.getDiagnostics()`.

## Invalidation and state

A new stream first publishes an opaque black clear packet at the configured dimensions. Dynamic previews retain the last valid image during pending decode for up to 500 ms after the latest seek, then replace it when a new frame arrives. Custom previews keep immediate invalidation on project edits, seeks and playback-state changes, with ordinary decode waits limited to 500 ms during playback or 100 ms while stopped. Missing input and REAPER rendering still clear the image. A clear packet removes retained content and does not indicate an available video source.

`status === 'ready'` identifies a valid source frame, including one retained during transient pending decode. Once cleared, `sourceWidth` and `sourceHeight` are `0` and `position` is `null`. Do not infer availability from packet arrival or black pixels alone. Clear the Canvas when the stream closes or reports an error.

| Error | Behavior |
| --- | --- |
| `VIDEO_RESOLUTION_LIMIT` | Reject oversized output on open. During a dynamic stream, clear the image and retain the connection until supported dimensions return |
| `VIDEO_PROFILE_CONFLICT` | Another consumer owns a different profile, without replacing its stream |
| `VIDEO_FRAME_PENDING` | Wait for a frame consistent with the current project state |
| `VIDEO_SOURCE_UNAVAILABLE` | No usable source frame or an invalid frame layout |
| `VIDEO_RENDER_BUSY` | Pause preview capture while REAPER is rendering, then resume |
| `VIDEO_PROJECT_CHANGED` | Close the old stream and require reopening for the current project |
| `VIDEO_HOST_UNSUPPORTED` | Reject opening when backend startup prerequisites are missing |

## Native I420

`format` defaults to `rgba8`. On verified Windows x64, macOS ARM64 and Linux x86_64 converters, `format: 'i420'` with `resolution: 'source'` copies the final native Y/U/V planes without an RGB conversion or new chroma subsampling. `openStream()` retains the requirement for widths divisible by 16 and even heights. Native `openSurface()` accepts valid even dimensions, copies each Y/U/V plane using its source row stride, and converts and scales on the GPU. CPU conversion restrictions are unchanged. The verified conversion is limited-range BT.709. Windows supports the verified FFmpeg 54 and 62 SSE conversions, Linux x86_64 supports the verified FFmpeg 60 SSE conversion, and macOS ARM64 uses the native NEON conversion. Other converter configurations retain RGBA. Other native formats, matrices and platforms reject the request with `VIDEO_FORMAT_UNSUPPORTED`. `capabilities.formats` advertises available paths. `sourceInfo.colorMatrix` can also identify BT.601/BT.2020 without enabling their I420 output.

An I420 packet contains full-size Y, then quarter-size U and V planes, packed without row padding. `frame.stride === frame.width` and `frame.bytes.byteLength === frame.width * frame.height * 3 / 2`. `info.maxBytes` remains the 40 MiB capacity limit. The descriptor carries `colorMatrix: 'bt709'`, `colorRange: 'limited'` and `colorConversion: 'reaper-sse' | 'reaper-neon'`. Use the indicated converter's integer rounding to reproduce CPU RGBA readback. The existing pixel budget applies to the equivalent RGBA dimensions. Format joins the first-consumer profile. Compatible size changes retain the I420 stream. Changes to native format, color matrix or converter that no longer satisfy the profile close it with `VIDEO_FORMAT_CHANGED` in video state. Reopen or fall back to RGBA.

The demo's web renderer keeps source-resolution I420 textures and draws into a surface sized in physical display pixels. Windows D3D11 uses linear YUV sampling before the BT.709 display matrix and RGB quantization, matching REAPER's OpenGL video window. The demo's Windows WebGL fallback retains area-weighted downscaling. macOS Metal and the macOS/Linux WebGL paths use bilinear RGB scaling after their host-specific integer YUV conversion. Original-pixel mode keeps 1:1 drawing. Display resizing does not change stream dimensions. Unsupported sources and WebViews retain RGBA rendering.

## Native presentation on Windows and macOS

For native presentation, fullscreen containers need a transparent `::backdrop`. Hide surrounding page content while fullscreen is active. Keep control spacing inside opaque backgrounds, using padding instead of exposed margins. The Demo applies these styles only to native presentation.

Windows x64 and macOS ARM64 support direct RGBA8 texture upload and the verified I420 conversions above, using D3D11 and Metal respectively. RGBA8 does not require another RGB conversion.

`getCapabilities().nativePresentation` reports `{available, backend: 'd3d11' | 'metal' | null}`. When available, `openSurface(options?)` opens a native video layer beneath the page's HTML content. It accepts the video profile options and shares capture and frame upload across native previews using the same profile. Windows I420 surfaces sample the shared planes in each viewport. For HD I420 using REAPER's DirectDraw output, a source-resolution D3D11 surface can select the BT.709 display path (`colorConversion: 'reaper-directdraw'`) independently of the project matrix. This profile is available only through native presentation. Hardware rounding can differ by one RGB level. Metal surfaces share the converted RGB texture. Keep the page backgrounds over the video region transparent and give overlaid controls their own backgrounds, as in the Demo. Existing `openStream()` pixel delivery, Windows worker delivery and Linux web rendering remain available.

The returned surface exposes frozen `info` with the attachment-time video descriptor and `renderer: 'native-d3d11' | 'native-metal'`, read-only `closed`, `error` and `readablePixels: false`, and asynchronous `setLayout({rect, clip, preserveAspect})`, `getDiagnostics()` and `close()`. `rect` and `clip` contain finite `x`, `y`, `width` and `height` in CSS viewport coordinates, without multiplying by device pixel ratio. Width and height must be nonnegative, and zero hides presentation. `preserveAspect` defaults to `true`. `on('close' | 'error', callback)` returns an unsubscribe function. There are no pixel events, `latest()` or `read()`, and `info` does not expose transport tokens or URLs. A surface is not a Canvas, and Canvas readback or webpage snapshots do not contain its native pixels.

Unavailable platforms return `VIDEO_PRESENTATION_UNAVAILABLE`. Each window can own one surface. Opening another returns `VIDEO_SURFACE_IN_USE`. Close the current surface before replacing its profile. Layout changes resize or clip presentation without changing capture dimensions. The demo's native fit mode requests `resolution: 'source'` and fits the display layout, preserving full source textures and allowing differently sized windows to share one profile. Custom mode still requests the chosen capture dimensions. Source-size changes retain the stream and native device. Textures are resized per reusable slot, and the Windows swapchain or Metal layer is retained when the viewport is unchanged. Use `getDiagnostics()` fields `sourceWidth` and `sourceHeight` for current native input dimensions.

Native presentation diagnostics describe submission and presentation activity, not physical screen scanout. `sourceUploadedFrames`, `sourceConvertedFrames` and `sourceDroppedFrames` describe the shared source and must not be summed across its surfaces. On Windows, `sourceConvertedFrames` counts each I420 frame once when it is first drawn. A detached surface reports `{closed: true}`, optionally with `errorCode` and `error` for a renderer failure. Source decoding, GPU load and display refresh still limit throughput. This path does not guarantee a fixed frame rate for an arbitrary number of windows.

Windows reports `presentationCounter: 'dxgi-observed'`. It counts newly observed source frames identified by DXGI presentation statistics, rather than successful `Present()` calls or differences between submission IDs. Unobserved intermediate frames are not inferred, so the count and FPS can be lower than the actual screen presentation rate. When statistics are unavailable, `presentationAvailable` is `false` and `presentedFrames`/`presentationFPS` are `null`. `hasFrame` remains independent of these counters. macOS counts Metal presentation callbacks. These measurements do not prove that every frame was visible on screen.

The demo retries native RGBA when the source does not support native I420. The Demo falls back to its web renderer when native presentation is unavailable or reports `VIDEO_RENDERER_UNAVAILABLE`. Windows uses its existing worker path, while macOS retains its web rendering fallbacks. The SDK does not silently turn an `openSurface()` result into a pixel stream. Applications choose their fallback explicitly. Native presentation does not enable WebView2 experimental texture streaming or alter ordinary window rendering settings.

When a viewport-fit Canvas preview reconnects after resizing, the Demo keeps the last drawn frame visible until the replacement draws its first frame.

## Dedicated video worker on Windows

`openWorkerStream(worker, options?)` moves the WebView2 video mapping directly to a dedicated `Worker`. The worker receives and uploads pixels without a full-frame copy on the page thread. The returned connection exposes `info`, `closed`, `error`, `on('data'|'error'|'close', callback)` and `close()`. Data events contain frame metadata, without `bytes`, `data`, `latest()` or `read()`. The existing `openStream()` continues to return immutable owned packets, with RGBA as the default.

Use the bundled `video-worker.js` and `worker-stream.js` as the worker protocol implementation. The SDK sends `{type:'reaweb-video', port, descriptor, buffer}`. The dedicated `MessagePort` carries `ready`, metadata `frame`, `error` and `closed` responses, and a `close` request. Shared slots are ACKed only after synchronous GPU upload or discard. Closing clears queued leases and transfers the mapping back in `closed` before detaching. Without a native mapping, the worker accepts the existing binary video transport. It does not impose a page-count limit.

Await `connection.close()` before terminating or reusing the worker. A failed worker or a one-second close timeout terminates that dedicated worker and detaches its consumer. Hidden-page and document cleanup also close these connections. The method requires WebView2 and rejects other engines with `UNSUPPORTED_FORMAT`. The demo uses it as the Windows web rendering path. Windows and macOS prefer native surfaces when available, while Linux retains web rendering.

## Ownership and limits

- `updateRate: 'auto'` follows the current project frame rate, including fractional rates such as 23.976 and 29.97, and updates while the stream remains open. The target is limited to 1–75 FPS, with a 30 FPS fallback when the project rate is unavailable. Numeric values retain the fixed 1–75 FPS mode, including fractional rates. Omitting the option selects `auto`, as does the Demo.
- State and diagnostics expose the requested `updateRate` and current `targetUpdateRate`. `stream.info.updateRate` is the numeric target at stream creation. Actual delivery depends on source decoding, host load and rendering. Displayed FPS is also limited by the display refresh rate. Auto and fixed modes are different shared profiles even when their current target rates match.
- One shared profile per current project. The first consumer selects width, height and update rate. Subsequent consumers must use the same values, otherwise `VIDEO_PROFILE_CONFLICT` is returned. Legacy custom defaults remain 1280×720. Each dimension is limited to 8192 and the RGBA payload to 40 MiB (10,485,760 pixels), covering UHD and DCI 4K. These are resource bounds, not resolution presets. Each of the three dynamic capture slots counts as 40 MiB against the existing 256 MiB shared producer-storage budget. The frame-rate range remains 1–75 FPS.
- Fit/source profiles adapt to final frame dimensions without reconnecting. Custom profiles keep their dimensions and optionally preserve aspect ratio with opaque black letterboxing. A dedicated video scheduler wakes the REAPER main thread independently of the extension timer. Internal video calls, pixel scaling/copying and publication remain on that thread. Frame transport uses the existing bounded binary stream.
- On Windows, an active project video stream enables high-resolution waits in the transport worker to reduce polling and ACK delays, with ordinary waits as a fallback. This leaves system timer resolution and the ACK protocol unchanged.
- During continuous playback, an unseen selected host frame is read before advancing video. It may lag the current query by at most one frame and must still match the project source, cached frame and monitor processing. Unchanged selected frames are identified by query index, source and monitor hashes, and frame timestamps, avoiding repeated conversion and copying. During normal or slower playback, auto reads distinct host frames directly when the project rate is at most 75 FPS. Fixed rates, faster playback and higher project rates retain deadline throttling. Unchanged or pending frames are retried at the next eligible scheduler wake without consuming another target interval. Stopped previews continue to refresh, and invalidation forces a fresh capture. This does not guarantee delivery of every source frame.
- REAPER converts native frames to its RGBA format, which uses BGRA byte order. The extension copies and scales those pixels into web RGBA8. This preserves host YUV conversion without selecting a separate color matrix or decoding media again. Source frame references remain local to capture and are released after copying. Only owned pixel bytes enter the stream.
- Video processing is activated on demand. The last consumer detaching releases the backend's scheduling state. An unclaimed stream expires after ten seconds. Closing, reloading and unloading use the existing stream cleanup.
- Hiding the App/page closes its video consumers, including a Dock transition that hides the page. Reopen when visible again. Opening from a hidden page rejects with `VIDEO_PAGE_HIDDEN`.
- Project changes close the old stream and increment its source epoch. Reopen after the project change. Consult state before presenting retained frames as current.

The backend depends on private REAPER functions and state. Compatibility across versions still requires host-level testing. RGBA and YV12 input paths are validated. YUY2 is delegated to the same host conversion path and has not had separate fixture acceptance. Native output scaling, display color management and GPU presentation can differ from the captured source pixels.

Run `tests/video_reaper.py` against a new disposable profile for host acceptance. Compile/unit tests alone do not establish host-level video compatibility.

## Demo display and fullscreen

The Demo automatically opens a fit-mode preview. Its Options menu combines display, output resolution and appearance controls. The default theme is dark, with a persistent light/dark choice. The native title-bar icon is hidden. Its video area reaches both page edges, while controls retain their own padding. The native fit preview retains source-resolution textures and updates only its display layout. Custom mode still selects capture dimensions. Web-rendered fit previews debounce resize and DPI changes and reopen only when resolved dimensions change. Source-size changes update the active stream without reconnecting. Display controls toggle fullscreen, aspect preservation and original pixel size. Double-clicking the video also toggles fullscreen. Escape and the overlay button exit it.

Original pixel size selects source output and maps one video pixel to one display pixel. Floating windows resize within the current display work area and restore their previous size when disabled. Docked windows keep Docker geometry. Oversized images remain scrollable instead of placing controls off-screen.

Fullscreen uses a native fullscreen container on Windows and Linux, and WebKit element fullscreen on macOS. Linux requires an X11 window manager advertising fullscreen support. Unsupported WebViews report a visible error rather than treating maximization as fullscreen. The Demo shows source, stream and Canvas box dimensions separately, along with target, capture, receive and draw rates. Canvas submissions are not physical display scanout measurements.

High-resolution raw RGBA transport increases memory and copy bandwidth. Original 4K output is supported within the stated budgets, but 60 FPS is not guaranteed. Benchmark the source, profile, display and audio-device combination in use.

`tests/video_cadence.py` checks capture, reception and drawing callbacks, not final screen presentation. Separately sample the screen using a constant-frame-rate numbered fixture, then analyze CSV or JSON containing `samples` with `tests/video_display.py`. Specify the frame-number column, loop period and minimum frame rate. The analyzer also rejects missing and backward frame transitions. Calibrate the sampler first and keep the test window visible and display awake. Software virtual displays do not establish physical-screen acceptance.

The demo provides EN / CN language switching. On the web rendering fallback, verified original-resolution I420 sources use one GPU texture upload and host-specific integer conversion. Windows I420 uses an OffscreenCanvas worker when supported, while macOS and Linux I420 use main-thread WebGL2. macOS retains uncompressed ImageBitmap presentation for RGBA when available. Other RGBA paths use an OffscreenCanvas worker where supported, with main-thread WebGL or Canvas 2D fallback. The demo transfers packet ownership to its worker after reading metadata and bounds pending worker submissions to six frames. Diagnostics distinguish transport sequence gaps from renderer queue drops. Hardware and display refresh rate still limit presentation. The demo opts into `videoHighRefreshRate: true` in `app.json`. On macOS, newly created video windows disable WebKit’s near-60-Hz page-update preference through a guarded private feature API when available. The default is `false`, and other platforms retain their existing scheduling. Reopen the window after changing this setting. Unsupported WebKit versions keep system scheduling, which can limit updates to about 72 FPS on a 144 Hz display. This setting does not change source FPS or guarantee display throughput. See the [WebKit scheduling issue](https://bugs.webkit.org/show_bug.cgi?id=221673).

Outside editable fields, unmodified Space uses the existing DAW keyboard routing for REAPER playback, enabled by the demo manifest. Fullscreen exit restores the floating window’s REAPER owner on Windows.
