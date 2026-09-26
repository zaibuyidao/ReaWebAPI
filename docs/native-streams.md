# Native Streams

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

## Native producer

Include [reaweb_stream.h](../src/public/reaweb_stream.h) and resolve the typed function pointers with REAPER `GetFunc`. ABI 1 provides `CreateFrameStream/PublishFrame`, `CreateAudioStream/PublishAudio`, `CreateSpectrumStream/PublishSpectrum`, `CreateMeterStream/PublishMeter`, `CreateWaveformStream/PublishWaveform`, `CreateBinaryStream/PublishBinary`, `CreateMIDIStream/PublishMIDI`, and `CloseStream`, all prefixed `ReaWeb_`.

Creation and close require REAPER's main thread. Set `size`, `abi_version`, `format`, `max_bytes`, and `capacity` in the descriptor. Set frame dimensions/stride or audio channels/sample rate as appropriate. Descriptor strings are copied. An optional registered service `owner` ties streams to extension lifetime. Stream names are unique ASCII letters, digits, `.`, `_`, `-`, up to 128 bytes. Handles never alias a later stream of the same name.

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

`audio.openStream` accepts `audio`, `spectrum`, `meter`, or `waveform`. Options are `source`, `fftSize` (power of two, 32–32768), and `updateRate` (1–120 Hz). Eight built-in producers may be active. They stop after the last consumer detaches. A consumer in another window keeps its producer alive.

`master` captures hardware output channels 0/1 after REAPER processing, including any other signals routed directly to those outputs. `input` captures hardware input 0/1. A mono device is duplicated to stereo. Capture uses a 16-slot PCM ring, maximum 8192 frames per block. Audio-thread work is limited to conversion and bounded copies. No WebView stall can block capture. Analysis runs on one native worker. Device sample-rate changes close affected streams with `UNSUPPORTED_FORMAT`; query devices and reopen.

`selected-track` captures the track selected at open, and `track:<GUID>` selects a specific track. Both use REAPER's **pre-FX audio accessor**, sampled on the main thread at playback position or edit cursor. Their `source` identifies the bound track and pre-FX tap. This is source-content analysis, not post-FX/live-input track metering. Use the existing `audio.getTrackMeter(track)` for REAPER's instantaneous track peak reading. Accessor sample acquisition follows native host scheduling, while processing and delivery use the independent worker/transport.

Spectrum contains `fftSize / 2 + 1` linear-amplitude bins per channel, using a Hann window. Meter contains channel peaks, channel RMS, momentary LUFS, short-term LUFS, integrated LUFS, then processed seconds. Silence is negative infinity for LUFS. [libebur128](https://github.com/jiixyj/libebur128) performs native EBU R128 analysis with bounded histogram storage. Capture discontinuities reset analysis history. Realtime waveform contains `[min, max]` for each channel of each of `min(256, fftSize)` buckets, oldest first. Overview and zoom queries reuse `audio.getWaveform(path, {start, duration, points})` and REAPER's native peak cache.

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

The [Native Stream Demo](../web/native-stream/README.md) consumes built-in analysis and third-party streams. ReaGBA integration belongs to its separate extension repository: `reaper.host.service('reagba')` provides controls/input and `reaper.stream.open('reagba.video')` provides frames. The emulator publishes at its own ~59.73 Hz cadence; the Lua launcher only opens the WebView.

A page with a Content Security Policy must allow `connect-src ws://127.0.0.1:*` for the native stream socket. Authorization remains bound to the page origin and a single-use native attachment ticket.
