# Realtime sources

`reaper.realtime` provides high-rate, latest-state subscriptions. Sources include `transport`, `track-controls` and `fx-parameters` on Windows, macOS and Linux. Rates are requests, not hard realtime guarantees.

```js
await reaper.lifecycle.ready;
const caps = await reaper.realtime.getCapabilities();
if (!caps.sources.transport.available) throw new Error('Transport sampling unavailable');
const subscription = await reaper.realtime.subscribe('transport', {
  updateRate: 120,
  onData(state) {
    render(state.position, state.playState, state.tempo, state.timeSignature, state.beatPosition, state.playRate);
  },
  onError(error) { console.error(error); }
});
await subscription.close();
```

`subscribe` accepts `updateRate` from 1 to 120 Hz, default 60, and optional `onData`, `onError`, `onClose` callbacks. It resolves after stream attachment. The returned object exposes `source`, `updateRate`, `latest()`, `closed`, `error` and idempotent async `close()`. `latest()` returns the last snapshot or null. Closing/navigation removes window-owned subscriptions. A failed attachment releases its producer. Up to 32 subscriptions share one manager per Runtime. Transport subscribers share a native scheduler. Sources are sampled once for all due subscribers. Each transport subscription publishes at its own requested cadence. The final transport unsubscribe stops the scheduler.

`getCapabilities()` lists source availability, schema and rate bounds without starting sampling. Initialization failures return `REALTIME_UNAVAILABLE`. Unknown sources return `REALTIME_SOURCE_NOT_FOUND`. `getDiagnostics()` reports window-owned subscriptions, the shared scheduler rate, source sampling/error counters and wakeups skipped during subscription management. Shared counters include other subscribing windows.

## Track controls

```js
const ref = await reaper.audio.getSelectedTrackRef();
if (ref) {
  const sub = await reaper.realtime.subscribe('track-controls', {
    tracks: [ref.guid], properties: ['volume', 'solo'], updateRate: 60,
    onData: state => render(state.tracks), onError: error => console.error(error.code)
  });
  // When the view is disposed:
  await sub.close();
}
```

Supply 1–16 unique braced GUIDs from the active project's ordinary tracks. Rows follow the requested order. Omit `properties` to receive all four fields. Volume is linear `D_VOL` (1 = 0 dB), pan is raw `D_PAN`, mute is boolean `B_MUTE`, and solo preserves the integer `I_SOLO`. Invalid lists, properties and rates reject with `INVALID_ARGUMENT`. A missing target rejects with `STALE_HANDLE`. Deleting a target or switching project closes the entire subscription with `STALE_HANDLE` or `PROJECT_CHANGED` respectively.

The first packet contains current values. Subsequent packets report changes only. Control-surface notifications invalidate the main-thread snapshot. Periodic reads cover missed notifications and script writes. Overlapping subscriptions share each due track/property read. Sampling follows REAPER's main timer and each subscription's rate limit. In the absence of notifications the periodic check is capped at 30 Hz. No track sampling runs without subscribers. Existing writes use `reaper.audio.setTrackValueLatest()`. Keep UI rendering separate from gesture writes.

The `track-controls.v1` stream carries a 40-byte little-endian header. Bytes 0–3 are `TC01`, byte 4 is the property mask (volume, pan, mute, solo from low to high), byte 5 is the track count and bytes 6–7 are reserved. Float64 sample time (host seconds) and duration (ms) start at offsets 8 and 16. Uint64 project epoch and shared sample sequence start at 24 and 32. Rows follow as packed float64 values in mask order. GUIDs come from the immutable subscription descriptor. Native Stream supplies packet sequence and producer drops.

Diagnostics report sampling and publishing rates averaged since the first sample, skipped periods, unchanged snapshots, query duration, sample-to-publication time and the latest trigger. These times exclude WebView delivery. Shared counts distinguish passes triggered by surface notifications from periodic or initial snapshots. Web arrival rate must be measured with the page clock.

## Transport snapshot

| Field | Meaning |
| --- | --- |
| `source` | `transport` |
| `position` | Unmodified `GetPlayPosition()` result in seconds, latency-compensated audible position |
| `playState` | `GetPlayState()` bitmask: 1 playing, 2 paused, 4 recording |
| `playRate` | `Master_GetPlayRateAtTime(position, 0)` playback rate multiplier |
| `tempo` | Tempo-map BPM at `position`, without the playback rate multiplier |
| `timeSignature` | `{ numerator, denominator }` at `position` |
| `beatPosition` | `TimeMap_timeToQN_abs(0, position)`, cumulative fractional project QN, not beat within measure |
| `sampledAt` | Host monotonic seconds at the beginning of the query |
| `sampleDurationMs` | Time spent reading and validating the snapshot |
| `sequence` | Per-subscription packet sequence, bigint |
| `sourceSequence` | Shared successful source query sequence, bigint |
| `projectEpoch` | Epoch updated when main-thread housekeeping observes an active-project identity/generation change, or on source restart, bigint |
| `projectChanged` | This query first published the latest observed project epoch. Compare epochs because latest-state delivery may omit that packet |
| `producerDropped` | Native stream buffer drop counter, bigint |

The source follows the active project. It reads state before and after position and discards a snapshot if state changes during the query. These API reads are not a host-atomic transaction. No interpolation is performed. Position is not replaced with the edit cursor when stopped. Position resolution depends on REAPER and its audio device/buffer configuration. A 120 Hz query/packet rate does not imply 120 distinct positions per second. Paused/stopped/device-closed states continue to be queried independently of the audio engine.

A dedicated native thread samples and publishes on all three platforms. Windows uses a waitable timer, macOS and Linux use monotonic deadline waits. Musical values are queried using `Master_GetPlayRateAtTime`, `TimeMap_GetTimeSigAtTime` and `TimeMap_timeToQN_abs` at the sampled position. Tempo ramps and meter changes are evaluated by REAPER. Project identity/generation tracking and subscription management stay on the main thread. `projectEpoch` is therefore a main-thread observation, not an atomic project identifier for the sampled transport. Sampling does not depend on Lua `defer`, `Runtime::tick()` or frame events. Missed periods are skipped. Scheduling and browser load can delay delivery. Consumers should use local receipt time to detect stale data and must not directly subtract `sampledAt` from `performance.now()`.

## Compatibility and extension points

The manager owns subscriptions, shared source reads, scheduling and cleanup. Source implementations own schemas, main-thread context refresh and thread-safe sampling semantics. A new source must establish its API threading contract before joining the native scheduler. JSFX producers remain managed by their existing instance lifecycle.

Transport uses a separate ordinary binary Native Stream with latest-value delivery. Its payload schema is `transport.v2`, with a 96-byte little-endian payload. The existing Stream ABI, v1/v2 headers, JSFX `packet.timing` and frame event contract are unchanged. Slow consumers may miss intermediate snapshots. This is not an ordered transport-event history.

| Offset | Encoding | Value |
| --- | --- | --- |
| 0 | 4 bytes | `RT02` |
| 4 | uint32 | Reserved, zero |
| 8 | float64 | Host monotonic sample time, seconds |
| 16 | float64 | Position, seconds |
| 24 | uint32 | Play state |
| 28 | uint32 | Bit 0: project epoch changed |
| 32 | uint64 | Project epoch |
| 40 | float64 | Query duration, seconds |
| 48 | uint64 | Source query sequence |
| 56 | uint64 | Reserved, zero |
| 64 | float64 | Playback rate |
| 72 | float64 | Tempo, BPM |
| 80 | float64 | Cumulative QN position |
| 88 | uint32 | Time signature numerator |
| 92 | uint32 | Time signature denominator |

The [demo](../examples/integrations/native-transport/README.md) defaults to English with a Chinese language switch. It compares 60/120 Hz subscriptions with frame events and exports timestamped CSV. Run `tests/realtime_reaper.py` for isolated-host measurements and `tests/jsfx_stream_recovery_reaper.py --realtime` for concurrent JSFX recovery acceptance. Both require explicit executable, extension and disposable output paths.

The runtime retains the `transport.v1` decoder for older producers. The four musical fields require v0.3.9.29. Repeat mode and loop boundaries remain available through existing APIs. Playing, paused and recording flags are already represented by `playState`.

## FX parameters

Pass the track GUID, FX instance GUID and 1–32 distinct zero-based parameter indices. Obtain the GUIDs in ReaScript with `GetTrackGUID(track)` and `TrackFX_GetFXGUID(track, fxIndex)`. The default `chain: 'main'` selects the regular or master track's main chain. Use `'input'` for record/input FX, `'monitoring'` with the master track GUID for monitoring FX, or `'take'` with an additional `take` GUID. Get the take GUID using ReaScript `GetSetMediaItemTakeInfo_String(take, 'GUID', '', false)` and the FX GUID using `TakeFX_GetFXGUID`. Nested Container FX are resolved by their own instance GUID within the requested chain. Check `getCapabilities().sources['fx-parameters'].chains` and `.containers` for host support.

```js
const sub = await reaper.realtime.subscribe('fx-parameters', {
  track: trackGuid, fx: fxGuid, parameters: [2, 0], updateRate: 60,
  onData: ({values}) => console.log(values)
});
```

The initial snapshot and subsequent changes contain `{index, normalized}` pairs in request order. Values come from `TrackFX_GetParamNormalized` or `TakeFX_GetParamNormalized` (0–1). Control-surface notifications mark state for a main-thread read. Periodic snapshots catch changes exposed by the host. Sampling depends on main-thread availability and can skip intermediate automation values. The requested rate (1–120 Hz, default 60) limits publication. Overlapping subscriptions share reads per pass. Reordering preserves the binding. Removing the FX, moving it outside the chain or losing a monitored parameter closes it with `STALE_HANDLE`. Switching project closes it with `PROJECT_CHANGED`. Invalid lists or out-of-range indices at subscription time reject with `INVALID_ARGUMENT`.

`fx-parameters.v1` uses little-endian `FP01`, a uint32 count, float64 host time (s) and duration (ms), uint64 project epoch and source sequence, then one float64 per requested index. Page diagnostics report `received`, `receiveHz` and `sequenceGaps` separately from host sampling, publishing and producer drops.

### Writing FX parameters

Resolve the same target once and reuse its project-bound reference for edits. Parameter indices are zero-based and values are normalized (0–1).

```js
const ref = await reaper.audio.getFxRef({track: trackGuid, fx: fxGuid});
await reaper.transaction.withUndo('Set FX mix', () =>
  reaper.audio.setFxParameterLatest(ref, mixIndex, 0.5));
```

Each parameter has one in-flight write and one pending value per page. A replaced pending write resolves with `superseded: true`. Use the existing begin/end Undo gesture for a drag and await all writes before ending it. Obtain a fresh reference after switching projects. Deleted or moved-out targets reject with `STALE_HANDLE`. Subscribe to observe the value the plug-in actually accepted.
