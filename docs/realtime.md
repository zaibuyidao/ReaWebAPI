# Realtime sources

`reaper.realtime` provides high-rate, latest-state subscriptions. v0.3.9.23 supports `transport` on Windows, macOS and Linux. Rates are requests, not hard realtime guarantees.

```js
await reaper.lifecycle.ready;
const caps = await reaper.realtime.getCapabilities();
if (!caps.sources.transport.available) throw new Error('Transport sampling unavailable');
const subscription = await reaper.realtime.subscribe('transport', {
  updateRate: 120,
  onData(state) {
    render(state.position, state.playState);
  },
  onError(error) { console.error(error); }
});
await subscription.close();
```

`subscribe` accepts `updateRate` from 1 to 120 Hz, default 60, and optional `onData`, `onError`, `onClose` callbacks. It resolves after stream attachment. The returned object exposes `source`, `updateRate`, `latest()`, `closed`, `error` and idempotent async `close()`. `latest()` returns the last snapshot or null. Closing/navigation removes window-owned subscriptions. A failed attachment releases its producer. Thirty-two subscriptions share one manager and one scheduler per Runtime. Sources are sampled once for all due subscribers. Each subscription publishes at its own requested cadence. The final unsubscribe stops the scheduler.

`getCapabilities()` lists source availability, schema and rate bounds without starting sampling. Initialization failures return `REALTIME_UNAVAILABLE`. Unknown sources return `REALTIME_SOURCE_NOT_FOUND`. `getDiagnostics()` reports window-owned subscriptions, the shared scheduler rate, source sampling/error counters and wakeups skipped during subscription management. Shared counters include other subscribing windows.

## Transport snapshot

| Field | Meaning |
| --- | --- |
| `source` | `transport` |
| `position` | Unmodified `GetPlayPosition()` result in seconds, latency-compensated audible position |
| `playState` | `GetPlayState()` bitmask: 1 playing, 2 paused, 4 recording |
| `sampledAt` | Host monotonic seconds at the beginning of the query |
| `sampleDurationMs` | Time spent reading and validating the snapshot |
| `sequence` | Per-subscription packet sequence, bigint |
| `sourceSequence` | Shared successful source query sequence, bigint |
| `projectEpoch` | Epoch updated when main-thread housekeeping observes an active-project identity/generation change, or on source restart, bigint |
| `projectChanged` | This query first published the latest observed project epoch. Compare epochs because latest-state delivery may omit that packet |
| `producerDropped` | Native stream buffer drop counter, bigint |

The source follows the active project. It reads state before and after position and discards a snapshot if state changes during the query. These API reads are not a host-atomic transaction. No interpolation is performed. Position is not replaced with the edit cursor when stopped. Position resolution depends on REAPER and its audio device/buffer configuration. A 120 Hz query/packet rate does not imply 120 distinct positions per second. Paused/stopped/device-closed states continue to be queried independently of the audio engine.

A dedicated native thread samples and publishes on all three platforms. Windows uses a waitable timer, macOS and Linux use monotonic deadline waits. `GetPlayPosition()` and `GetPlayState()` are the only getters used there, both explicitly documented as thread-safe in [Cockos Host Functions](https://www.cockos.com/reaper/sdk/vst/vst_ext.php#host_functions). Project identity/generation tracking and subscription management stay on the main thread. `projectEpoch` is therefore a main-thread observation, not an atomic project identifier for the sampled transport. Sampling does not depend on Lua `defer`, `Runtime::tick()` or frame events. Missed periods are skipped. Scheduling and browser load can delay delivery. Consumers should use local receipt time to detect stale data and must not directly subtract `sampledAt` from `performance.now()`.

## Compatibility and extension points

The manager owns subscriptions, shared source reads, scheduling and cleanup. Source implementations own schemas, main-thread context refresh and thread-safe sampling semantics. A new source must establish its API threading contract before joining the native scheduler. The initial source is transport. JSFX producers remain managed by their existing instance lifecycle.

Transport uses a separate ordinary binary Native Stream with latest-value delivery. Its payload schema is `transport.v1`, with a 64-byte little-endian payload. The existing Stream ABI, v1/v2 headers, JSFX `packet.timing` and frame event contract are unchanged. Slow consumers may miss intermediate snapshots. This is not an ordered transport-event history.

| Offset | Encoding | Value |
| --- | --- | --- |
| 0 | 4 bytes | `RT01` |
| 4 | uint32 | Reserved, zero |
| 8 | float64 | Host monotonic sample time, seconds |
| 16 | float64 | Position, seconds |
| 24 | uint32 | Play state |
| 28 | uint32 | Bit 0: project epoch changed |
| 32 | uint64 | Project epoch |
| 40 | float64 | Query duration, seconds |
| 48 | uint64 | Source query sequence |
| 56 | uint64 | Reserved, zero |

The [demo](../examples/integrations/native-transport/README.md) defaults to English with a Chinese language switch. It compares 60/120 Hz subscriptions with frame events and exports timestamped CSV. Run `tests/realtime_reaper.py` for isolated-host measurements and `tests/jsfx_stream_recovery_reaper.py --realtime` for concurrent JSFX recovery acceptance. Both require explicit executable, extension and disposable output paths.
