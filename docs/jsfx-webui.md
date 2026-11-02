# JSFX Web UI

[简体中文](jsfx-webui.zh-CN.md)

Native JSFX editors with a valid Web UI association are hosted automatically. No global preference or INI setting is required. The association's `enabled` field defaults to `true`.

The module uses the existing WebView2, WKWebView or WebKitGTK runtime, browser profile, `reaweb://<appId>` origin and Service bridge. REAPER continues to own the JSFX DSP and parameter state. No Lua backend or additional extension is required.

## Association

Place a JSON file beside the JSFX with the same basename. Identification uses REAPER's `fx_type=JS` and `fx_ident`, never the display name.

```text
Effects/Author/Utility/
  Utility.jsfx
  Utility.json
  index.html
  app.js
  jsfx-webui.mjs
```

```json
{"schema":1,"JSFX_WebUI":{"entry":"index.html","enabled":true,"width":640,"height":420,"dawKeyboard":true}}
```

`entry` must resolve to an HTML file beneath the JSFX directory. The JSFX must resolve beneath `Effects`. Missing, disabled or invalid associations leave the native editor intact. `width` and `height` request the initial editor area (100..2048, defaults 640×420) for standalone FX windows. Docked windows retain host geometry. An optional `app.json` in the HTML directory follows the normal App identity contract. Without an explicit `app.json.id`, the module derives a stable App ID from `fx_ident`. Copy `SDK/jsfx-webui.mjs` beside the page to use the helper below.

`dawKeyboard` is the initial Space routing setting on Windows, macOS and Linux, defaulting to `true`. Non-boolean values invalidate the association. When enabled, unmodified Space outside editable fields uses REAPER’s current main-section shortcut binding while editable fields and IME composition retain Space.

Both ordinary Apps and JSFX can override the current state with [`reaper.window.setDawKeyboard`](runtime-api.md#space-routing), without writing configuration. Page reload rereads the respective JSON. JSFX ignores `app.json.dawKeyboard`, whose default for ordinary Apps is `false`.

## Parameter service

After `reaper.lifecycle.ready`, use `reaper.host.service('jsfx')`. Requests bind to the calling Hosted WebView. Ordinary Apps and External Clients cannot select or impersonate a binding.

The helper requires `runtime.jsfx.protocolVersion === 1`. Use `connectJSFX()` and `jsfx.params`. All extended capabilities use the same `jsfx` service.

After the capability handshake, independent event subscriptions register concurrently. Initial editor size and parameters are then read concurrently. Connection waits for both, applies size before parameters and retains newer events over older snapshots. Failed connections invalidate callbacks and release completed and late subscriptions. Native presentation still follows size initialization and the caller's initial render. See [startup timing](devtools.md#startup-timing).

During initialization, the native Runtime handles `getCapabilities`, `getEditorSize` and `getParamState` in the main-thread tick that dispatches each request, without an additional Service completion tick. This applies on Windows, macOS and Linux. JavaScript calls remain asynchronous, with the same session validation and response format.

On Windows, macOS and Linux, `STALE_SESSION` during these initial reads triggers a capability refresh and at most two retries of the read-only snapshots. Results from different sessions are not applied together, and parameter writes are not retried. Repeated session changes or other errors fail the connection and release subscriptions. This behavior requires the updated `jsfx-webui.mjs` bundled with the UI. Updating the native extension does not replace App-local SDK copies.

All JSFX replies use `{ok:true,sessionId,data}` or `{ok:false,sessionId?,error:{code,message,details?}}`. The helper unwraps replies and rejects business errors with `code` and `details`. `getCapabilities` bootstraps the instance session. Subsequent requests include `sessionId`, and active editor operations also include `activation`. Revisions are decimal strings, compared as `BigInt`.

| SDK / service | Input → result data |
| --- | --- |
| `getCapabilities` | Returns protocol version, instance session, capability revision, features and limits. |
| `params.getState` / `getParamState` | Returns `{sessionId,fxIdent,count,parameters,revision,activation,active}`. |
| `params.beginEdit` / `beginParamEdit` | `{index}` → `{gestureId}`. Opens one managed Undo gesture. |
| `params.set` / `setParam` | `{index,normalized,gestureId?}` → `{parameter,revision}`. Finite normalized values in [0,1] only. |
| `params.endEdit` / `endParamEdit` | `{gestureId}` → `{ended}`. The helper then refreshes the snapshot. |
| `params.beginEditGroup` / `beginParamEditGroup` | `{indices}` → `{gestureId}`. Mutually exclusive with single-parameter gestures. |
| `params.setMany` / `setParams` | `{gestureId,values:[{index,normalized}]}` → `{applied,revision}`. Validates every value before writing. |
| `params.endEditGroup` / `endParamEditGroup` | `{gestureId}` → `{ended}`. One Undo unit for the group. |
| `present` | `{activation}` → `{presented}` after rendering current host state. |
| `paramsChanged` | `{sessionId,parameters,revision,activation}` for this instance only. |
| `visibilityChanged` | `{sessionId,active,activation,paramState?}`. Reactivation includes a fresh snapshot. |
| `getDiagnostics` | Instance status, native channel availability, active gesture and presentation time. |

`params.snapshot` is an immutable cache populated before connection resolves. Subscriptions return `Promise<Dispose>`, where Dispose is an idempotent asynchronous function. The helper tracks sessions, activations and gesture IDs. Direct service clients must do so themselves. JSFX methods require `invoke`, not `send`.

Without an EEL native declaration, capabilities report `status:'parameterOnly'`, parameter groups enabled and `nativeChannel:false`. Extended methods reject with `UNSUPPORTED`. Participating effects can expose declared streams, schema-checked data states, acknowledged commands and reliable binary resources through the same service. See the [native channel](jsfx-native-channel.md) for transport and synchronization contracts.

Parameter metadata includes `index`, `name`, `ident`, `min`, `max`, `mid`, `defaultNormalized`, `step`, `smallStep`, `largeStep`, `toggle`, `enum`, `value`, `normalized` and `formatted`. Unsupported metadata is `null`. The native API supplies normalized defaults. `mid` is the host midpoint, not a default. The normalized setter preserves REAPER's parameter shaping.

### Metadata availability

The same contract applies on Windows, macOS and Linux. `params.snapshot` and `params.onChange` retain metadata when value-only events or write replies arrive. `params.getState()` rereads metadata from the host.

| Field | Source and unavailable behavior |
| --- | --- |
| `enum` | ReaWebAPI reads the loaded JSFX engine's `sx_parmIsEnum`, `sx_getParmVal` and `sx_getParmDisplay`. An array of `{value, label}`, or `null` for non-enums or unavailable metadata. `value` is the raw parameter value. |
| `formatted` | `TrackFX_GetFormattedParamValue` / `TakeFX_GetFormattedParamValue`. This is the current display text, which may be an enum label, not a list of choices or a unit. |

The [official JSFX engine API](https://github.com/justinfrankel/jsfx/blob/main/jsfx/jsfx_api.h) is separate from public TrackFX/TakeFX APIs. ReaWebAPI obtains the opaque instance from its validated native editor, checks engine ABI `0xf0f01091`, and queries already-loaded options without parsing source, creating another instance or changing parameter values. This depends on the native JSFX dialog convention. An unavailable or ambiguous editor, unsupported ABI or mismatched parameter layout returns `null`. `toggle` remains the independent public host result, including for two-option enums.

For `slider1:0<0,2,1{Off,Low,High}>Mode`, `enum` is `[{value:0,label:'Off'}, {value:1,label:'Low'}, {value:2,label:'High'}]`. Labels retain engine order, including duplicates and empty strings. Empty declarations skipped by the engine are not reconstructed. Single-option enums are supported. File-selection sliders, custom `@gfx` menus and REAPER's Bypass/Wet/Delta parameters are not engine enums and return `null`.

Enumeration is read on connection, editor reactivation and `params.getState()`. After recompiling an effect, call `getState()` to refresh its metadata. No instance pointer or enum table is cached in the native reader. Reads allow up to 1,024 options per parameter, 4,096 options and 128 KiB of label text per snapshot, and 4,094 UTF-8 bytes per label. The serialized snapshot also respects the host service's 1 MiB message limit. A table that cannot be returned completely within these limits is `null`.

```js
const rows = state => state.parameters.map(p => ({
  index: p.index, name: p.name,
  display: p.formatted ?? String(p.value), choices: p.enum ?? null
}));
console.table(rows(jsfx.params.snapshot));
const stop = await jsfx.params.onChange(state => console.table(rows(state)));

// Write a selected raw enum value through the normalized parameter service.
const p = jsfx.params.snapshot.parameters[0];
const option = p.enum?.[1];
if (option && p.max > p.min) {
  await jsfx.params.beginEdit(p.index);
  try { await jsfx.params.set(p.index, (option.value - p.min) / (p.max - p.min)); }
  finally { await jsfx.params.endEdit(); }
}
```

```js
import { connectJSFX } from './jsfx-webui.mjs';

await reaper.lifecycle.ready;
const jsfx = await connectJSFX(reaper, error => showError(error.message));
render(jsfx.params.snapshot);
const dispose = await jsfx.params.onChange(render);
```

Keep the actively dragged control at its local pointer position. Continue receiving host state, then render the calibrated host value after `endEdit()`. Serialize successive gestures and scope completion callbacks to their gesture, so a delayed release cannot repaint a new drag. Start a new relative drag from the displayed value. Let native range inputs manage their own pointer tracking. Reserve explicit pointer capture for custom controls such as knobs. Parameter metadata retains the host's raw floating-point values. Display `formatted` when available.

During a single-parameter gesture, the helper keeps at most one write in flight and one pending value. Call `endEdit()` on pointer release, cancellation and keyboard completion. Each `params.set` outside a gesture creates a discrete Undo operation. Its Promise reports applied or superseded values. Discrete writes and group batches use a bounded serial queue. `setMany` is not sample-atomic. A failed host setter after an earlier write returns `PARTIAL_WRITE` with applied indices, ends the gesture and refreshes the helper snapshot. Navigation, invalidation and a 30-second inactive gesture timeout end edits. Initialization and reload never write parameter defaults.

Host parameter access runs on the main thread. The limit is 256 JSFX sliders plus REAPER Bypass, Wet and Delta. Reordering resolves the same FX GUID within its original chain. Moving a take to another track invalidates its binding.

## Editor size

ReaWebAPI v0.3.8.31 adds `features.editorSize`. `jsfx.editor.size` is populated before `connectJSFX()` resolves. `getSize()` refreshes it. `setSize(width,height)` accepts integer content dimensions of 100..8192 CSS pixels and requires an active editor. `onSizeChange(listener)` observes the shared preference, including while the page is cached. Results contain `{width,height,customized,revision}`. Before the first explicit request, `customized` is false and dimensions come from the association, whose existing minimum-size behavior is unchanged.

Explicit sizes can grow or shrink the host frame. They exclude native toolbars, meters and borders. The host converts CSS pixels to native units, including macOS points and Windows/Linux display scaling. Docked editors retain host geometry. The preference is shared by `fx_ident` and resolved HTML entry within the REAPER resource profile, persists across restarts, and is applied before chain/floating presentation. Display names do not define sharing. Layout and zoom rendering remain the page's responsibility.

```js
const jsfx = await connectJSFX(reaper);
const renderSize = size => { if (size.customized) renderLayout(size.width, size.height); };
renderSize(jsfx.editor.size);
await jsfx.editor.onSizeChange(renderSize);
// In a user resize or scale handler:
await jsfx.editor.setSize(1035, 239);
```

Direct Service clients use `getEditorSize`, `setEditorSize({width,height,activation})` and `editorSizeChanged({sessionId,size})`. Both methods require `sessionId`. Reactivation includes `editorSize` in `visibilityChanged`. Render it before acknowledging `present`. The helper waits for pending size requests before presentation. Older runtimes report `UNSUPPORTED` through the new helper.

## Page and editor lifetime

Native-control concealment on Windows, macOS and Linux checks visibility before querying control bounds and resolves meter handles once per scan. Toolbar, meter and hosted-page visibility rules are unchanged.

Pages are created on demand. Closing an editor or switching FX retains up to four hidden pages, released after 60 seconds idle. FX deletion or project invalidation releases them immediately. Reactivation refreshes host state and waits for rendering and `present` before showing the page. The helper manages activation tokens and presentation acknowledgements.

Preloaded WebView containers contain only a blank page. A new JSFX instance still requires page loading, session binding, initial state reads and first-frame preparation. Removing and re-adding the effect creates a new instance and cannot reuse the deleted instance's page. The native channel uses [bounded staging programs](jsfx-native-channel.md#transport) to reduce initialization cost independently of the container pool.

On Windows, JSFX first-frame readiness uses an internal JPEG capture. The encoded image is discarded and never displayed, so UI rendering quality is unchanged. Presentation still waits for successful capture and the page's `present` acknowledgement. Ordinary Apps retain PNG captures. macOS and Linux retain their existing presentation paths.

Use `jsfx.active` and `onVisibilityChange` to pause timers, stream subscriptions and animations. Deactivation ends parameter gestures and discards pending drag values. Direct Service clients must render `visibilityChanged.paramState`, then acknowledge its `activation`. Initialization failure or a 15-second presentation timeout restores native content.

Track, master, take, input and monitoring FX are supported, including nested containers, chain editors and floating editors. Floating/chain transitions reuse the same page and restore its shared size preference, falling back to the association size. REAPER controls docking. Bindings validate the project, owner and FX GUIDs, not window titles or last-touched FX.

The page replaces only the associated JSFX content area. Presets, Param, routing, UI switching, wet/dry and bypass retain native behavior. No duplicate toolbar is created. Native side meters remain visible. `options:no_meter` removes their reserved width. Both modes retain the native bottom inset.

Windows batches native content-control hiding within the current JSFX editor and coalesces redraw requests, falling back to individual hides if the batch fails. The WebView child, native toolbar and meters are excluded. Other editor windows are unaffected. Control restoration on detach and first-frame presentation guards retain their existing behavior. macOS and Linux continue to hide controls individually.

Windows uses WebView2, macOS uses WKWebView, and Linux uses the SWELL/X11 WebKit embedding path. The adapter targets REAPER 7.82 and the REAPER 7 layout family. Window recognition involves host-private controls. Unrecognized structures retain the native editor. Reopen after initialization failure to retry. The module does not convert `@gfx` code.

On Windows, use v0.3.8.28 or later for FX-chain WebViews to participate correctly in native dialog focus navigation. Associated JSFX and page code need no changes.

## Optional DSP capabilities

Install the matching `SDK/jsfx/reaweb_jsfx.inc` beside the effect. The SDK archive includes three self-contained examples under `SDK/jsfx/examples`: `analyzer`, `curve` and `resource`. Copy an example directory beneath REAPER's `Effects` directory. The analyzer shows raw PCM, min/max waveform and a Hann-window spectrum. Its test signal affects telemetry only. The curve example uses CAS and native Undo. The resource example imports/exports an embedded byte asset and invokes a DSP command.

Add `native` inside `JSFX_WebUI`. Its `transportParam` is a zero-based host parameter index of an explicitly reserved JSFX slider. Never repurpose an existing automatable parameter. The EEL `options:gmem` and `reaweb_jsfx_init(base,memory)` must match the declaration. Existing effects retain their gmem name and choose an unused reserved range.

```json
{
  "protocolVersion": 1,
  "gmem": "Author_Effect_v1",
  "base": 0,
  "transportParam": 1,
  "streams": [{"id":"output","kind":"spectrum","source":"postDSP","channels":2,"fftSize":2048}],
  "dataStates": [{"id":"curve","schemaVersion":1,"schema":{"type":"array","maxItems":4,"items":{"type":"number","minimum":0,"maximum":1}},"persistent":true,"undo":true}],
  "commands": [{"id":"sum","args":{"type":"array","maxItems":8,"items":{"type":"number","minimum":0,"maximum":1}},"result":{"type":"number","minimum":0,"maximum":8}}],
  "resources": [{"id":"asset","format":"bytes","maxBytes":32768,"persistent":true,"undo":true}]
}
```

IDs match `[A-Za-z][A-Za-z0-9_.-]{0,127}` and are unique within their category. EEL numeric IDs are declaration-array indices, starting at zero. Object schema fields are encoded in lexicographic key order. All fields are required. Arrays and UTF-8 strings begin with their element/byte count. Booleans use 0/1. Numeric bounds must be finite. Schema nesting is limited to eight levels, encoded values to 4096 doubles, and JSON values to 64 KiB. Revisions are unsigned 64-bit decimal strings, with two 32-bit limbs inside EEL.

| SDK | Service method | Contract |
| --- | --- | --- |
| `streams.open(id, {updateRate}?)` | `openStream`, then existing Stream transport | Returns the standard `ReaWebStream`. Instances/documents cannot open one another's protected producers. |
| `dataState.get(id)` | `getDataState` | `{id,revision,schemaVersion,value,available}`. Suspended DSP may return cached data with `available:false`. |
| `dataState.set(id,value,{expectedRevision,requestId?})` | `setDataState` | Returns an operation. Stale revisions fail with `CONFLICT` and the current snapshot. |
| `commands.release()` | `releaseCommands` | Release this document's continuous actions and owned notes. |
| `commands.invoke(id,args,{requestId?})` | `invokeCommand` | Validated, bounded command executed by the participating effect. |
| `operations.get/find/cancel/onChange` | `getOperation/findOperation/cancelOperation`, `operationChanged` | Observe execution and recover a lost response without resending side effects. |
| `resources.beginUpload/writeChunk/commitUpload/abortUpload` | `beginResourceUpload/commitResourceUpload/abortResourceUpload`, binary transport | Staged replacement. `writeChunk` accepts `Uint8Array`, with sequential byte offsets. |
| `resources.openDownload/readChunk/closeDownload` | `openResourceDownload/closeResourceDownload`, binary transport | Immutable revision snapshot, random byte-range reads and explicit cleanup. |

`requestId` deduplicates the same operation and payload within one instance session. Reusing it with another payload is an error. Up to 16 operations may be pending. A bounded 256-record cache retains completed operations for 60 seconds. Operations remain `queued` with `blockedReason:'DSP_UNAVAILABLE'` when the processor is not scheduled. Only DSP acknowledgement yields `succeeded` or `cancelled`. A command exceeding `maxExecutionMs` gets `deadlineExceeded:true`. Cancellable handlers receive a cancellation request. The operation and its Undo record remain live until acknowledgement or instance invalidation, since a deadline alone cannot prove that no side effects occurred. Cancellation of a running non-cancellable command is rejected.

Streams support `audio`, `spectrum`, `waveform`, `meter`, `binary` and `midi`. The first four consume explicitly published interleaved PCM. Spectrum is channel-interleaved linear amplitude with a Hann window. Waveform contains min/max pairs per channel and point. Audio retains raw samples for oscilloscopes and phase plots. Meter uses the existing `reaper.audio.decodeMeter` layout. Binary/MIDI carry plugin-defined byte records and must declare their interpretation at the application layer. They do not imply hardware MIDI input. Owned note input uses the command helpers below.

Once opened through `jsfx.streams.open`, a stream retains its object and listeners across DSP session replacement and recoverable transport closure, including preset reset, recompilation and audio-device recovery. Recovery replaces `info` and discards old buffered packets. `read()` and `latest()` return `null` while reconnecting, with `closed === false`. Valid streams with no data remain connected. A disconnected stream waits for suspended DSP at one-second intervals. Other recovery failures use up to six attempts with 100–4000 ms backoff, reset after receiving data. Terminal failure emits `error` and `close` and calls the `connectJSFX` error handler. Explicit `close()`, editor deactivation and page cleanup are terminal. Reopen streams on editor activation when needed. FX deletion or an invalid project stops recovery. No page reload or custom session reconnection loop is required. Existing effects must update their bundled `jsfx-webui.mjs` to use this behavior.

JSFX streams negotiate layout version 2 without changing existing Stream packets. `packet.timing` supplies `clockEpoch`, `firstSample`, `sourceDropped`, `position`, `playState` and `discontinuity`. Clear analysis/display history on discontinuities. The descriptor includes the selected source, channel labels, sample rate and FFT size. Analysis publication is packet-aligned at the negotiated 1–120 Hz rate, not a fixed sample hop. The default is 30 Hz. Dropped telemetry is expected under backpressure and never blocks waiting for a page. Trigger detection, precomputed spectral layouts and application-specific MIDI records can be implemented by the effect using audio/binary streams.

Resource formats are raw bytes and finite IEEE float32 little-endian values. The declared maximum is further bounded by the EEL bank capacity. Uploads require a lowercase SHA-256 checksum and expected revision at commit. Binary chunks have CRC32, sequence/offset acknowledgement and bounded retry. Old DSP data remains intact until the complete staging bank passes CAS and swaps at a block boundary. Downloads return a fixed snapshot with its checksum and metadata. Metadata is a JSON object limited to 768 UTF-8 bytes. File selection can use the existing file APIs or a browser file input. Format decoding/resampling belongs outside `@block`, with explicit sample-rate/channel metadata. This release embeds registered resources through `@serialize`. It does not introduce filesystem-reference relocation or an audio-file decoder.

The existing Stream server carries the private `RWR1` resource mode. Control JSON never contains resource bytes. A transfer has at most one 256 KiB chunk in flight and expires after 30 seconds without progress. Limits are two transfers and 32 MiB staging per instance, with 128 MiB shared by the Runtime. Reservations include transfer buffers and in-flight overhead, so the largest accepted payload can be smaller than 32 MiB. DSP banks are separately preallocated by the effect. Losing page ownership closes streams and uncommitted transfers. Accepted state/resource operations remain queryable within the same DSP session. New DSP generations invalidate old tickets, operations and snapshots. An instance without a cached page or retained operation/transfer is released after 60 seconds. Reopening then establishes a fresh session.

### EEL integration

Reserve 32768 local words for the SDK, separate from audio buffers and both banks of every state/resource. The gmem protocol reserves `[base, base + 2170944)`, with 32 live instance slots. A namespace/range collision fails closed. The native bridge never uses browser-supplied addresses.

```eel
options:gmem=Author_Effect_v1
import reaweb_jsfx.inc
slider1:1<0,2,0.001>Gain
slider2:0<-70368744177664,70368744177664,1>-ReaWebAPI transport v1
@init
reaweb_jsfx_init(0,65536);
// Register state/resource banks here, initializing defaults only on return value 1.
@slider
slider2=reaweb_jsfx_handshake(slider2);
@block
slider2=reaweb_jsfx_handshake(slider2);
kind=reaweb_jsfx_service();
// Handle kind 1 using reaweb_jsfx_id, reaweb_jsfx_length and reaweb_jsfx_value(i).
// Reply with reaweb_jsfx_reply(code, data, length). Zero means success.
@serialize
reaweb_jsfx_serialize(0,state_mask,resource_mask);
slider2=reaweb_jsfx_handshake(slider2);
```

`reaweb_jsfx_state(id,bankA,bankB,length,capacity,schemaVersion,readOnly)` and `reaweb_jsfx_resource(id,bankA,bankB,length,capacity,format)` return 1 for new banks, 2 for retained banks and 0 for invalid declarations. Resource formats are 1=bytes, 2=float32. Use `reaweb_jsfx_state_data`, `reaweb_jsfx_resource_data` and `reaweb_jsfx_resource_length` after each `reaweb_jsfx_service()` call. Do not retain a bank pointer across a block boundary. `reaweb_jsfx_state_update` stages a bounded DSP-origin state update and increments its revision.

`reaweb_jsfx_stream_active(id)` reports an active native subscription. Use it to skip optional capture work. Publishing an unsubscribed stream is a no-op. Publish at most 4096 values with `reaweb_jsfx_publish(id,data,length,channels,firstSample)`. Buffers and copies must be bounded. For asynchronous custom commands, do a bounded amount of work per block, check `reaweb_jsfx_cancelled()` and acknowledge cancellation with code 6. Reset private in-flight work when `reaweb_jsfx_session_changed` is set, then clear the flag. Do not send UI commands to a different FX, run JSON parsing, access files or wait on the audio thread.

`reaweb_jsfx_serialize` appends its own versioned records to the effect's existing serialization stream. Masks select durable states/resources by numeric ID. Exact doubles, float32 payloads, bytes and metadata round-trip through project/preset/Undo storage. Loading validates records before swapping active banks. Preserve schema versions deliberately when updating an effect. The reserved handshake value is transient and never authorizes a restored/copied instance by itself.

Connection and reconnection use a shared-memory challenge and targeted parameter readback. They do not write host parameters, create Undo entries or mark the project dirty. Unscheduled DSP waits for execution. Actual parameter edits and durable state commits retain normal Undo behavior. REAPER may record editor closure or window-layout changes, as it does for native editors.

### Continuous commands and MIDI

Declare `continuous:true,cancellable:true` for held actions, without `undo` or `persistent`. `commands.release()` requests cleanup for the calling document. The SDK releases on blur, deactivation and disposal, and rejects new continuous calls after blur until focus returns. Native document destruction also releases ownership. Custom DSP handlers read `reaweb_jsfx_released_owner` after `reaweb_jsfx_service()`: -1 means no release, 0 means all owners from the old session, and a positive value identifies one owner. Match it against the `reaweb_jsfx_owner` captured when starting an action.

In a continuous command handler, use `reaweb_jsfx_note_on(channel,note,velocity)` and `reaweb_jsfx_note_off(channel,note)`. They return 1 on success and 0 on rejection/send failure. The 128-entry ledger pairs UI notes, preserves the current MIDI bus and releases only owned notes. A bus/channel/note already held by another UI owner is rejected. Failed note-offs are retried on subsequent blocks. No global all-notes-off or routing changes are sent. MIDI 1.0 cannot distinguish an external source using the same channel/note, so dedicate a channel when independent ownership is required. Cleanup waits for DSP scheduling if the effect is bypassed or the device is stopped.
