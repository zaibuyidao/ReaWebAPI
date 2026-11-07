# Video Preview

Install ReaWebAPI v0.3.9.28, then run `reawebapi-video-preview.lua`. Open a project containing video and click **Open stream**. This example is not included in the ReaPack package.

Supported video hosts are the verified Windows REAPER 7.84 x64, macOS REAPER 7.84 arm64 and Linux REAPER 7.82 x86_64 module builds. Unknown builds report `VIDEO_HOST_UNSUPPORTED`.

The Demo uses `updateRate: 'auto'` to follow the project frame rate, capped at 60 FPS. It shows the current target and actual preview FPS. Fixed integer rates remain available through the API.

The Canvas displays final project frames and clears invalidated output. The initial black clear packet is not an available source frame. **Close stream** releases this consumer. Hiding the page closes its stream. Reopen after project changes. Backend state, errors and capture/clear counters appear below the preview.

See [project video documentation](../../../docs/video-preview.md) for the shared resolution policy, lifecycle and host limitations.
