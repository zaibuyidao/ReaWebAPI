# ReaWebAPI

- Add final video frame dimension discovery and source, viewport-fit and custom resolution profiles, with explicit pixel-budget errors and shared-profile protection.
- Support RGBA video output up to 40 MiB per frame, including UHD/DCI 4K, with bounded transport storage, bilinear resizing and full-resolution preview drawing.
- Add native element fullscreen support on Windows, macOS and supported Linux window managers.
- Update the video demo with automatic preview, a unified Options menu, dark/light themes, EN/CN switching, fullscreen and aspect controls, original pixel sizing, Space playback and separate resolution/frame-rate diagnostics.
- Reduce video copies with Windows worker delivery, Linux native memory and macOS local resources. Add opt-in original I420 output and GPU conversion for verified Windows x64, macOS ARM64 and Linux x86_64 BT.709 converters, with source-resolution textures and viewport-sized GPU drawing. Allow video windows to opt out of macOS WebKit’s near-60-Hz scheduling preference. Restore the floating preview owner after Windows fullscreen.
- Avoid false video invalidation when repeated host playback positions catch up with the audio clock. Keep capture timing tied to completed frames instead of empty polling.
- Validate adjacent cached video frames against their own source identity to avoid rejecting valid frames during asynchronous decoding.
- Prevent slow video socket consumers from starving other previews, with a capacity-bounded outgoing frame pool and reserved publication space.
- Fix a video queue race that could skip an earlier frame during concurrent publication without reporting a buffer overflow.
- Add Windows D3D11 and macOS Metal native video surfaces that share frame upload and RGB conversion across previews, while retaining HTML controls and existing pixel streams.
