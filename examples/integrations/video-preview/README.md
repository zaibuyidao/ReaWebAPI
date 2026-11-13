# Project video preview

Requires ReaWebAPI v0.3.9.40 or newer. Run `reawebapi-video-preview.lua` in REAPER. No capture FX or companion plugin is installed.

The preview starts automatically. A single Options menu contains display, output and light/dark appearance settings. Dark is the default, and the native title-bar icon is hidden.

On Windows and macOS, the demo prefers native D3D11 and Metal presentation respectively when available. Its default fit mode keeps source-resolution textures and fits only the display layout, sharing upload and RGB conversion between previews with the same profile. Web-rendered fit mode resolves the actual video area's physical dimensions through `reaper.video`. Original resolution preserves the final project frame dimensions. Source-size changes update original and fit previews within the existing stream. Fit output stays within the requested viewport bounds. Changing those bounds or the output mode still reopens the stream. Custom resolution retains fixed width/height control. Source, output and display dimensions and capture/receive/draw rates are shown separately.

The video reaches both page edges. Display options toggle fullscreen, aspect preservation and original pixel size. Double-click also toggles fullscreen. Escape exits it. Original pixel size adjusts floating windows within the display work area, leaves Docker geometry alone and permits scrolling when the frame is larger than the viewport.

The web renderer keeps at most six ordered packets and 128 MiB, waits for two packets or about 16 ms at startup, and drops the oldest packet on overflow. Invalidation, timestamp jumps, errors and closing clear queued frames. Fit resizing is debounced and preserves other consumers' shared profile ownership.

Fullscreen availability depends on the native WebView and, on Linux, an X11 window manager supporting fullscreen. Higher resolution does not guarantee a particular frame rate. See the [video API](../../../docs/video-preview.md) for budgets, waiting states and source-size changes.

Use EN / CN to switch the interface language. The web rendering path on Windows x64, macOS ARM64 and Linux x86_64 uses native I420 for original-resolution previews and one GPU texture upload for verified BT.709 sources, with platform-specific RGBA fallback. Windows transfers native video memory directly to its OffscreenCanvas worker and returns slot ownership after upload, avoiding a full-frame page-thread copy. macOS and Linux I420 use main-thread WebGL2. macOS RGBA uses ImageBitmap when available. Other RGBA paths prefer an OffscreenCanvas worker, with main-thread WebGL or Canvas 2D fallback. Frame dimensions travel with each packet, and textures adapt without reconnecting. I420 drawing follows the physical display size, with 1:1 output in original-pixel mode.

Unmodified Space outside editable fields controls REAPER playback through the existing DAW keyboard routing.

The demo opts into `videoHighRefreshRate: true` in `app.json`. On macOS, this disables WebKit's preference for near-60-Hz page updates when the relevant system feature is available. Reopen the window after changing this setting. It does not change the source frame rate or guarantee presentation beyond the display's refresh rate.

Native I420 surfaces accept even frame dimensions, including widths that are not multiples of 16. Source row padding is removed before GPU upload. CPU conversion and pixel-stream restrictions are unchanged.

Native surfaces expose layout and diagnostics through `reaper.video.openSurface()`, without JavaScript-readable pixels. HTML controls remain above the video. Canvas readback and webpage snapshots do not include native video pixels. Custom output still selects fixed capture dimensions. Pixel consumers use each packet's `width`, `height` and `stride`, since `info` retains attachment-time dimensions. Dynamic `info.maxBytes` is a capacity limit, while `frame.bytes.byteLength` is the valid payload length. Shared presentation does not guarantee a particular frame rate or window count.

The demo retries native RGBA when the source does not support native I420. If native presentation is unavailable or its renderer fails, the demo falls back to web rendering. Windows native display rates use observed DXGI presentation statistics, and show `—` when those statistics are unavailable. Successful GPU submissions are not treated as displayed frames.
