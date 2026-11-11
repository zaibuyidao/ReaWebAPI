# Native Streams

[External Clients](external-clients.md) attach to this stream hub with session-bound tickets. Binary packets, acknowledgements, backpressure and existing WebView Origin checks retain their contracts.

`reaper.host` routes to Lua, `reaper.host.service(name)` to native services, `reaper.events` to state notifications, and `reaper.stream` to continuous binary data. Existing APIs retain their contracts.

## Consumer

```js
await reaper.lifecycle.ready;
const stream = await reaper.stream.open('producer.video');
const draw = () => {
  const frame = stream.latest(); // Cached bytes, no RPC.
  if (frame) uploadTexture(frame.bytes, stream.info);
  if (!stream.closed) requestAnimationFrame(draw);
};
stream.on('close', error => console.log(error.code));
stream.on('error', error => console.error(error));
draw();
await stream.close(); // Detach this consumer.
```

Open performs one control request and creates a persistent loopback WebSocket. Binary packets never pass through Lua, JSON frame serialization or `Runtime::tick()`. The transport thread serves all streams. Single-use attachment tickets expire after 10 seconds and bind the connection to the requesting App origin and document lifetime. There are at most 128 streams and 64 consumers per Runtime.

Latest-type streams replay their most recent published packet to new consumers, including when the producer is paused. `latest()` returns the last received packet or `null`. `read()` dequeues one Audio/MIDI packet and otherwise returns `latest()`. A packet contains `sequence`/`frameId` and `producerDropped` as `bigint`, a timestamp in producer clock seconds, a byte view, and a `Uint8Array` or `Float32Array` data view. Retained packets are immutable by convention. Applications must bound any references they retain.

`on('data'|'close'|'error', callback)` returns a synchronous listener disposer. `close()` detaches only the current consumer. Closing or navigating the WebView detaches its consumers automatically. Closing a producer clears consumer caches and delivers `STREAM_CLOSED` or `EXTENSION_UNLOADED`. Missing streams, invalid arguments, unsupported formats, timeouts and transport failures report typed errors. Consumer close is idempotent.

`stream.getDiagnostics().transport` reports successful connections, rejected handshakes, expired attachment tickets and the last handshake rejection code. These counters cover the Runtime lifetime and contain no connection tickets. The field is absent before transport initialization.

## Native producer

Include [reaweb_stream.h](../src/public/reaweb_stream.h) and resolve the typed function pointers with REAPER `GetFunc`. ABI 1 provides `CreateFrameStream/PublishFrame`, `CreateAudioStream/PublishAudio`, `CreateSpectrumStream/PublishSpectrum`, `CreateMeterStream/PublishMeter`, `CreateWaveformStream/PublishWaveform`, `CreateBinaryStream/PublishBinary`, `CreateMIDIStream/PublishMIDI`, and `CloseStream`, all prefixed `ReaWeb_`.

Creation and close require REAPER's main thread. Set `size`, `abi_version`, `format`, `max_bytes`, and `capacity` in the descriptor. Set frame dimensions/stride or audio channels/sample rate as appropriate. Descriptor strings are copied. An optional registered service `owner` ties streams to extension lifetime. Stream names are unique ASCII letters, digits, `.`, `_`, `-`, up to 128 bytes. Handles never alias a later stream of the same name.

`REAWEB_PIXEL_I420` identifies the optional built-in [project-video](video-preview.md) layout. External `CreateFrameStream` producers still accept only RGBA8/BGRA8. The ABI version and descriptor size are unchanged.

Publish copies into preallocated storage and returns immediately. A stream has one producer. Publish makes no host/WebView calls, performs no allocation, acquires no mutex and never waits for consumers. Concurrent producers receive `PRODUCER_BUSY`. Audio callbacks may publish directly. FFT, decoding and other expensive processing belong on a worker. Producers define their own clocks.

Use `ReaWeb_SetServiceShutdown(service, callback)` to stop and join producers before Runtime removal, while provider functions are still callable. Normal extension shutdown follows the same order: stop/join producers, close streams, unregister the service, release resources. The [complete synthetic producer](../tests/native_stream_extension.cpp) demonstrates this lifecycle.

| Kind | Format and layout | Backpressure |
| --- | --- | --- |
| Frame | RGBA8 or BGRA8, exact `stride × height` bytes, up to 8192 pixels per dimension | Discard obsolete frames, never regress to an older publication |
| Audio | Interleaved Float32 PCM, `channels`, `sampleRate`, maximum `blockFrames` | Bounded FIFO, reject incoming blocks when full |
| Spectrum | Float32, bins interleaved by channel, `fftSize`, `binHz = sampleRate / fftSize` | Latest analysis wins |
| Meter | Float32, producer-defined layout. Built-in layout below | Latest analysis wins |
| Waveform | Float32 min/max peaks. Built-in layout below | Latest analysis wins |
| Binary | Arbitrary bytes | Latest packet wins |
| MIDI | Byte event packet. Built-in layout below | Bounded FIFO, reject incoming events when full |

Buffers have 2–64 slots, at most 16 MiB per packet and 256 MiB of allocated producer storage per Runtime, including closed buffers still referenced by transport. Transport retains at most one in-flight packet plus eight queued packets/16 MiB per consumer. Latest types keep one queued replacement. Audio/MIDI drop incoming packets when their transport or consumer FIFO fills. Native overrun returns `BUFFER_FULL`; `producerDropped`, sequence gaps and consumer `dropped` expose loss. Underrun returns `null`, without synthesized silence or replay. Reopen a stream to reset its consumer queue. Acknowledgments provide backpressure without blocking producers. A stalled connection is terminated after 30 seconds.

## Built-in audio and MIDI

```js
const spectrum = await reaper.audio.openStream('spectrum', {
  source: 'master', fftSize: 2048, updateRate: 30
});
const midi = await reaper.system.openMIDIInput(-1);
```

`audio.openStream` accepts `audio`, `spectrum`, `meter`, or `waveform`. Options are `source`, `aggregate` (boolean, default `false`), `fftSize` (power of two, 32–32768), and `updateRate` (1–120 Hz). Eight built-in producers may be active. They stop after the last consumer detaches. A consumer in another window keeps its producer alive.

`master` captures hardware output after REAPER processing, including signals routed directly to those outputs. `input` captures hardware input. Meter preserves the available channel layout up to 32 channels. Other analysis kinds retain channels 0/1 and duplicate mono input to stereo. Capture uses a 16-slot PCM ring, maximum 8192 frames per block. Audio-thread work is limited to conversion and bounded copies. No WebView stall can block capture. Analysis runs on one native worker. Device sample-rate changes close affected streams with `UNSUPPORTED_FORMAT`; query devices and reopen.

`selected-track` captures the track selected at open, and `track:<GUID>` selects a specific track. Both use REAPER's **pre-FX audio accessor**, sampled on the main thread at playback position or edit cursor. Their `source` identifies the bound track and pre-FX tap. This is source-content analysis, not post-FX/live-input track metering. Use the existing `audio.getTrackMeter(track)` for REAPER's instantaneous track peak reading. Accessor sample acquisition follows native host scheduling, while processing and delivery use the independent worker/transport.

### Aggregate source

```js
const stream = await reaper.audio.openStream('spectrum', {
  source: 'selected-track', // Or 'track:<GUID>'.
  aggregate: true, fftSize: 2048, updateRate: 30
});
```

For track sources, `aggregate: true` recursively includes the bound track, folder children with enabled parent sends, and audio receive sources. Muted receives and MIDI-only routes are excluded. Each source track contributes once, including in cyclic routing. Routing and mute/solo changes are checked at each sample block. Muted tracks and branches routed through muted folders are excluded. Solo selects the contributing source branches, including solo defeat and solo-in-place send paths. A muted root returns silence.

All accessors read at the same project time, sample rate, channel layout and block size (stereo for non-Meter streams). Native code sums their PCM before the existing FFT, Peak/RMS/LUFS and waveform analysis. The single stream retains floating-point sums above `1.0`, without normalization, limiting, averaging or gain compensation. Item/take/lane playback is determined by REAPER's accessor PCM.

Audio, spectrum and waveform streams with the same track, aggregation mode, sample rate and update rate share source sampling. Meter streams share a separate continuous source for the same track, aggregation mode, sample rate and playback-reset policy, independently of update rate. Aggregate reads yield between source tracks when the host tick budget is exhausted and publish only complete sums at one captured project position. Bridge dispatch has a separate time budget so slow source reads cannot starve connection and detach requests. A single REAPER accessor call cannot be interrupted. Track accessors discard isolated non-finite or Float32-overflow PCM blocks, then resume with a raw PCM sequence gap and reset analysis history. Eight consecutive invalid blocks close the source with `NATIVE_ERROR`. Other analysis failures close the affected stream without stopping other producers or later streams.

This is synchronized **pre-FX source PCM aggregation**, not post-FX, pre-fader, post-fader or track output capture. Track/send gain, pan, phase and channel remapping are not applied. It creates no FX, sends, tracks or Undo entries. `aggregate: true` rejects `master` and `input` with `INVALID_ARGUMENT`. Omitted or `false` preserves the existing source behavior. The stream descriptor identifies this mode as `track:<GUID>:pre-fx:aggregate-source`. Deleting the bound track or switching projects closes the aggregate stream.

### Analysis payloads

Spectrum contains `fftSize / 2 + 1` linear-amplitude bins per channel, using a Hann window. Realtime waveform contains `[min, max]` for each channel of each of `min(256, fftSize)` buckets, oldest first. Overview and zoom queries reuse `audio.getWaveform(path, {start, duration, points})` and REAPER's native peak cache.

#### Built-in Meter / Loudness Analyzer

```js
const meter = await reaper.audio.openStream('meter', {
  source: 'selected-track', updateRate: 30,
  forceMono: false, resetOnPlaybackStart: true, integratedMode: 'playback-only'
});
meter.on('data', ({ data }) => {
  const values = reaper.audio.decodeMeter(data, meter.info.channels);
  console.log(values.lufsIntegrated, values.sampleClipCount[0].toString());
});
await reaper.audio.resetMeter(meter.info.name);
```

`forceMono` (default `false`), `resetOnPlaybackStart` (default `true`) and `integratedMode` are Meter-only options. `integratedMode` defaults to `playback-only`: RMS-I, LUFS-I and LRA accumulate only during transport playback. `continuous` accumulates those histories whenever valid PCM arrives. Both modes leave realtime measurements and peak/clip histories unchanged. Playback-start reset remains independent, so use `resetOnPlaybackStart: false` to retain continuous history across starts.

The built-in payload is `Float32Array(13 * C + 14)`, where `C = stream.info.channels` is 1–32. The first `7*C+14` values keep the previous offsets. The following table describes this compatible prefix. ABI 1, `CreateMeterStream`, `PublishMeter` and third-party producer-defined layouts are unchanged.

| Offset | Field | Function / unit |
| --- | --- | --- |
| `0 … C-1` | `samplePeak[C]` | Maximum absolute sample per channel in the publication interval, linear amplitude |
| `C … 2*C-1` | `truePeak[C]` | Cockos interpolated peak per channel in the publication interval, linear amplitude |
| `2*C … 3*C-1` | `channelRms[C]` | Per-channel RMS in the publication interval, linear amplitude |
| `3*C … 4*C-1` | `sampleClipCount[C]` | Approximate compatibility count of samples with absolute amplitude strictly greater than 1 |
| `4*C … 5*C-1` | `truePeakClipCount[C]` | Approximate compatibility count of True Peak above 1, at most once per input sample |
| `5*C … 6*C-1` | `channelMaxSamplePeak[C]` | Per-channel historical Sample Peak, linear amplitude |
| `6*C … 7*C-1` | `channelMaxTruePeak[C]` | Per-channel historical True Peak, linear amplitude |
| `7*C + 0` | `rmsMomentary` | 400 ms summed-channel RMS, dBFS |
| `7*C + 1` | `rmsIntegrated` | Cockos RMS-I: mean energy of overlapping 400 ms RMS windows sampled every 100 ms, dBFS |
| `7*C + 2` | `maxRmsMomentary` | Maximum RMS-M since reset, dBFS |
| `7*C + 3` | `lufsMomentary` | K-weighted 400 ms loudness, LUFS |
| `7*C + 4` | `lufsShortTerm` | K-weighted 3 s loudness, LUFS |
| `7*C + 5` | `lufsIntegrated` | Cockos gated integrated loudness since reset, LUFS |
| `7*C + 6` | `loudnessRange` | `loudnessRangeHigh - loudnessRangeLow`, LU |
| `7*C + 7` | `maxLufsMomentary` | Maximum LUFS-M since reset, LUFS |
| `7*C + 8` | `maxLufsShortTerm` | Maximum LUFS-S since reset, LUFS |
| `7*C + 9` | `maxSamplePeak` | Maximum of `channelMaxSamplePeak`, linear amplitude |
| `7*C + 10` | `maxTruePeak` | Maximum of `channelMaxTruePeak`, linear amplitude |
| `7*C + 11` | `processedSeconds` | Processed PCM frames divided by sample rate since reset, seconds |
| `7*C + 12` | `loudnessRangeLow` | Gated short-term loudness 10th percentile, LUFS |
| `7*C + 13` | `loudnessRangeHigh` | Gated short-term loudness 95th percentile, LUFS |

RMS-M and LUFS-M use 400 ms windows, LUFS-S uses 3 s. Windows and maxima advance every 100 ms of PCM, independently of publication. RMS-I accumulates linear window energy, including startup zero padding, and becomes available after four integrated steps. Global RMS sums channel energy without averaging or K-weighting. Silent RMS/LUFS and incomplete M/S windows are negative infinity. LRA, peaks, clips and duration start at zero. LRA bounds start at -100 LUFS before mono calibration. The prefix clip counts remain Float32 compatibility views. Use the exact suffix through `audio.decodeMeter` for cumulative counts.

True Peak uses the Cockos 32-tap windowed-sinc interpolation, with three fractional phases below 96 kHz and one phase at higher rates. Its filter delay is 16 samples. `truePeakClipCount` counts the maximum across the delayed sample and interpolation phases once per input sample. Sample clipping and True Peak clipping remain independent. Sample Peak and its channel history reuse libebur128. Global peak maxima are derived from the channel histories.

LUFS-M/S/I share Cockos K-weighting, with 400 ms/3 s windows stepped every 100 ms. LUFS-I retains exact energy sums in 0.1 LU bins with absolute and relative gating. LRA collects LUFS-S every 100 ms after the 3 s window fills, uses Cockos absolute/-20 LU relative gates, and updates its bounds after at least 20 gated observations. Its 10th/95th percentile selection and bin-edge values follow the official JSFX.

Loudness channels follow REAPER order L/R/C/LFE/surrounds. Below six channels every channel has unit weight. From six channels onward LFE is excluded and surrounds use sqrt(2) amplitude weighting for all LUFS measurements. `forceMono` subtracts 3 dB from global RMS/LUFS, their maxima and LRA bounds, without downmixing PCM.

Track Meter uses the track channel count. Aggregate Meter uses the largest channel count among the root and its audio-connected upstream tracks, independent of mute/solo, and sums corresponding channel indices. Routing channel remapping remains outside this pre-FX source aggregation contract. Hardware Meter uses the capture channel count, including mono. When the hardware hook omits its channel count, both creation and capture use the device's enabled input or output channel count. Sources wider than 32 channels expose their first 32. Channel-layout changes close Meter streams with `UNSUPPORTED_FORMAT`, requiring reopen with fresh metadata.

The exact counter suffix starts at `T = 7*C+14`. It contains six channel arrays: Sample Clip low/middle/high, then True Peak Clip low/middle/high. Each count is encoded as `low + middle*2^24 + high*2^48`, using 24/24/16-bit nonnegative integers that Float32 represents exactly. Native counters saturate at `2^64-1` rather than wrapping. `audio.decodeMeter(data, C)` returns named measurements and `sampleClipCount`/`truePeakClipCount` as `bigint[]`. Convert a count to a decimal string for JSON. It is a synchronous SDK decoder for this built-in layout, with no RPC or changes to generic stream decoding. Third-party Meter payloads remain producer-defined.

Track and aggregate meters analyze contiguous forward PCM during playback and retain history while stopped without rereading the edit cursor. Seek, loop wrap, source-content changes and accessor refresh reset history. Repeated refresh reports from aggregate accessors whose audio time range remains empty do not reset history. Adding or removing media still resets it. Playback start resets by default. Disabling `resetOnPlaybackStart` retains history across ordinary stop/resume, but never across detected discontinuities. Hardware master/input meters continue analyzing captured PCM while stopped. `integratedMode` controls only RMS-I/LUFS-I/LRA accumulation. Track/aggregate sources do not produce new PCM while stopped, including in continuous mode. Capture gaps and dropped PCM reset history. Sample-rate or channel-layout changes close Meter streams with `UNSUPPORTED_FORMAT`, requiring reopen with fresh metadata and history.

`samplePeak` is the existing Channel Peak measurement. LUFS-M/S/I use the single `lufsMomentary`/`lufsShortTerm`/`lufsIntegrated` path. Historical peak maxima are `maxSamplePeak` and `maxTruePeak`. All meter reset triggers share one analyzer lifecycle, clearing filters, windows, maxima and accumulated duration together. `processedSeconds` is the analyzer's processed PCM frame count divided by its sample rate. Reading or publishing does not advance it.

`audio.resetMeter(name)` queues a reset for a built-in meter opened by this window. A stopped meter publishes its cleared state. Already delivered packets remain immutable. Reopening starts fresh. `updateRate` controls only meter publication, never PCM ingestion or historical results. `samplePeak`, `truePeak` and Channel RMS retain their publication-interval semantics. Other analysis kinds and source tap/routing semantics retain their existing behavior.

MIDI selects an input index or `-1` for all inputs. Each packet starts with 16 little-endian bytes: `uint32 device`, `int32 sampleOffset`, `uint32 length`, reserved `uint32`, followed by MIDI bytes. The low 16 device bits are the input index; other bits retain REAPER's control-input flags. Sequence is REAPER's event sequence and timestamp is its project position. Note on/off and CC use their ordinary MIDI bytes. REAPER's recent-input history is sampled in bounded main-thread batches. Events larger than its 1024-byte read limit are omitted. This observer does not open disabled devices or change routing. `devicesChanged` supplies low-frequency device snapshots.

## Platform additions

| API | Contract |
| --- | --- |
| `system.getDevices()` | Active REAPER audio configuration/channel names and MIDI input/output names and presence |
| `system.getDisplays()` | Native monitor/work-area coordinates, effective DPI, scale and primary display. macOS uses AppKit coordinates, Windows physical desktop units, Linux GDK logical units |
| `debug.getDiagnostics()` | Adds process CPU seconds, logical processor count, stream allocations and counters |
| `fs.watch(path, callback, {recursive})` | Native worker snapshots every 250 ms, with create/change/delete/rename notifications and file identity matching. A watched file follows renames within its parent. Eight watches, 10000 entries each, no symlink traversal. Short-lived changes may coalesce. Overflow is explicit and requires refreshing the directory |
| `clipboard.readBinary(format)` / `writeBinary(format, bytes)` | Custom `ReaWebAPI.Binary:<MIME>` clipboard format, up to 16 MiB. Not a conversion to OS image clipboard formats |
| `system.schedule(callback, {delay, interval})` | Native one-shot or repeat timer. Milliseconds, maximum 24 hours, repeat minimum 10 ms. Missed periods skip, callbacks execute through native host scheduling. Returns an async disposer |

File watch creation resolves after its baseline. File watchers and timers release on navigation/window close. Native timer callbacks use [reaweb_tasks.h](../src/public/reaweb_tasks.h), with optional service ownership. Timers are for delayed/background work, never a substitute for the producer's frame clock. Existing text clipboard, native drag/drop, window geometry, title/icon, focus, visibility and dock APIs retain their behavior.

The [Native Stream Demo](../examples/integrations/native-stream/README.md) consumes built-in analysis and third-party streams. ReaGBA integration belongs to its separate extension repository: `reaper.host.service('reagba')` provides controls/input and `reaper.stream.open('reagba.video')` provides frames. The emulator publishes at its own ~59.73 Hz cadence; the Lua launcher only opens the WebView.

A page with a Content Security Policy must allow `connect-src ws://127.0.0.1:*` for the native stream socket. Authorization remains bound to the page origin and a single-use native attachment ticket.

## Project video

`reaper.video` exposes shared project video frames through Native Streams. Only verified REAPER builds are supported. See [Video Preview](video-preview.md) for installation, API, rendering and platform limits.
