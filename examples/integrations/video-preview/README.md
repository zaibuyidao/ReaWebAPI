# Video Preview

Install ReaWebAPI v0.3.9.32 or newer, then run `reawebapi-video-preview.lua`. Open a project containing video and click **Open stream**. This example is not included in the ReaPack package.

Video compatibility is checked by normalized module fingerprint on Windows x64, macOS arm64 and Linux x86_64. Changes confined to the normalized metadata fields do not require a new version entry. Unmatched modules report `VIDEO_HOST_UNSUPPORTED`.

The Demo uses `updateRate: 'auto'` to follow the project frame rate, capped at 75 FPS. It shows the current target and average capture FPS. Fixed fractional or integer rates remain available through the API.

The Canvas displays final project frames and clears invalidated output. The initial black clear packet is not an available source frame. **Close stream** releases this consumer. Hiding the page closes its stream. Reopen after project changes. Backend state, errors and capture/clear counters appear below the preview.

The renderer draws an ordered queue of up to six packets through continuous `requestAnimationFrame` callbacks. Startup waits for two packets or about 16 ms, and overflow drops the oldest packet. This smooths frame arrival at the cost of bounded buffering. Explicit video invalidation, backward timestamps, forward jumps exceeding elapsed arrival time by over 150 ms, errors and closing clear queued work. Displayed FPS remains limited by browser and display refresh rates.

See [project video documentation](../../../docs/video-preview.md) for the shared resolution policy, lifecycle and host limitations.
