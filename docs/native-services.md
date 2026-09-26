# Native events and Host Services

`reaper.host.send(message)` retains the Lua Backend route and message format. `reaper.host.service(name)` addresses built-in or third-party native services. `reaper.events` carries Runtime and REAPER state events. All three can coexist. WebView lifetime and native communication are maintained by REAPER's native timer, independently of Lua `defer`.

## JavaScript

```js
const test = reaper.host.service('test');
const callback = payload => console.log(payload);
const stop = await test.on('changed', callback);
const stopUnload = await test.on('unloaded', error => console.log(error.code));
const result = await test.invoke('ping');
test.send('message', {text: 'hello'});
await test.off('changed', callback);
await stop(); // safe to repeat
await stopUnload();
```

`service(name)` creates a local proxy without requiring registration. `invoke(method, payload = null)` resolves the current registration and returns a Promise. `send(method, payload = null)` returns `void`, without waiting for a business result. Dispatch errors go to the Runtime error log. Names contain 1–128 ASCII letters, digits, `_`, `-` or `.`. Payloads and results are JSON, at most 1 MiB. Services share existing bridge queue limits. Native completion/event queues additionally cap at 2048 entries and 16 MiB, with at most 1024 pending invokes globally.

`on` returns `Promise<Dispose>`, matching `events.on`. `off` removes every registration of the same callback for that event. Service events are discrete, ordered and have no snapshot replay. `unloaded` is reserved. Unregister rejects pending invokes with `EXTENSION_UNLOADED`, emits `unloaded` to interested consumers and removes subscriptions. Subscribe again after registering a replacement. A stale native handle cannot target a replacement. Window close and navigation cancel pending work and remove subscriptions. An unfinished invoke times out after 30 seconds of native execution. Cancellation does not roll back side effects.

The built-in `runtime` service implements `getInfo`, returning `{version, serviceABI}`. It uses the same registry as third-party extensions. State queries and controls retain existing Mirror APIs.

## Native integration

Use the versioned C ABI in [reaweb_service.h](../src/public/reaweb_service.h). Resolve `ReaWeb_RegisterService`, `ReaWeb_UnregisterService`, `ReaWeb_CompleteServiceCall` and `ReaWeb_EmitServiceEvent` with REAPER's `GetFunc`. These are native extension APIs, not Mirror or Lua vararg APIs. The [test extension](../tests/native_service_extension.cpp) is a complete example.

The packaged SDK places the public header and example together in `native/`. Compile the example against the official REAPER extension SDK, adding that directory to the include path.

Register a unique name with a callback table containing its byte size, ABI version and user context. Registration returns a status and opaque 64-bit handle. Duplicate names return `SERVICE_EXISTS`. Runtime copies the table, not the context. Callback arguments are borrowed during the call. Copy them before asynchronous work.

Registration, unregistration, request callbacks and cancellation callbacks run on REAPER's main thread. Wrong-thread registration/unregistration returns `MAIN_THREAD_REQUIRED`. Request ID zero means send. For invoke, return `REAWEB_OK` and complete exactly once, immediately or later. Return another status for dispatch failure. Completion and event functions are thread safe, copy their JSON and queue delivery to the native timer. They never execute REAPER APIs or JavaScript on the worker. Event window ID zero broadcasts, otherwise only that window's subscribers receive it.

Callbacks must return promptly. Move non-REAPER heavy work to extension workers. Runtime bounds queued delivery per timer pass, but cannot preempt extension callbacks.

Before extension unload, unregister every service, then stop and join its workers. Optional `on_cancel` handles invoke timeout, window close/navigation and service unregistration. Runtime never retains a callback for a removed registration. Keep context and module alive throughout callbacks, never unload the module from a callback, and stop using these function pointers before ReaWebAPI unloads. Repeated unregister is harmless. Late completion returns `REQUEST_GONE` or `EXTENSION_UNLOADED`.

Errors include `SERVICE_NOT_FOUND`, `METHOD_NOT_FOUND`, `INVALID_ARGUMENT`, `EXTENSION_UNLOADED`, `MAIN_THREAD_REQUIRED`, `TIMEOUT`, `SERVICE_EXISTS`, `QUEUE_LIMIT` and `SERVICE_ERROR`. Native functions return status codes. JavaScript invokes reject with `error.code` and `error.message`.

## Native state events

New sources use the existing `events.on/off` signatures and disposal behavior. A shared monitor manager activates on the first subscription and stops polling after the last. Selection, track, marker and transport callbacks invalidate snapshots. A 100 ms fallback observes changes without suitable callbacks. Track and marker scans advance at most 64 entries per timer pass within the Runtime budget. Delivery coalesces state changes, so these describe current state rather than an edit journal. Multiple windows share each monitor. Positions use the bounded polling cadence, not a binary streaming channel.

| Event | Payload beyond `projectEpoch`, `revision`, `available` |
| --- | --- |
| `trackSelectionChanged` | `count`, `invalidated: true`. Compares the complete ordered selection, including empty and same-count replacements. |
| `trackStateChanged` | `count`, `invalidated: true`. Track list/order, names, color, mute, solo, record arm and TCP/MCP visibility. |
| `transportChanged` | `state`, `playing`, `paused`, `recording`, `position`, `cursor`, `rate`, `loop`. Times are seconds. |
| `projectChanged` | `activeProject`, `projects: [{id, path, dirty}]`, `changeCount`, `generation`. Tab additions/removals, switches, file loads and path/dirty changes. IDs are opaque session identities, not Mirror handles. |
| `markersChanged`, `regionsChanged` | `count`, `invalidated: true`. Includes position, length, name, ID and native color changes. |
| `currentRegionChanged` | `region: {id, start, end, name, color}` or `null`. Uses play position while playing, otherwise edit cursor. |
| `loopPointsChanged`, `timeSelectionChanged` | `start`, `end` in seconds. |

An initial snapshot is emitted after subscription. `available: false` means required host functions are unavailable. Unchanged snapshots do not increment revision. Project changes invalidate partial scans. Payloads exclude FX, routing, state chunks and full track/marker lists. Refresh only the data your UI needs:

| State/control | Existing Mirror APIs |
| --- | --- |
| Track/selection | `CountTracks`, `CountSelectedTracks`, `GetTrack`, `GetSelectedTrack`, `GetTrackName`, `GetTrackColor`, `GetMediaTrackInfo_Value`, `SetMediaTrackInfo_Value` |
| Transport | `OnPlayButton`, `OnPauseButton`, `OnStopButton`, `CSurf_OnRecord`, `GetPlayState`, `GetCursorPosition`, `GetPlayPosition`, `GetSetRepeat`, `Master_GetPlayRate`, `CSurf_OnPlayRateChange` |
| Project | `EnumProjects`, `IsProjectDirty`, `GetProjectStateChangeCount` |
| Timeline | `CountProjectMarkers`, `EnumProjectMarkers3`, `GetLastMarkerAndCurRegion`, `GetSet_LoopTimeRange2` |

Existing events retain their original names and payloads. Clipboard, native drag/drop, window/dock/focus, platform/architecture and device queries continue through existing Runtime namespaces and Mirror functions. Service/event methods never enter the 730-function REAPER Mirror.

## Verification

With `BUILD_TESTING=ON`, CTest includes registry lifecycle/error/worker tests, monitor state comparisons, JavaScript bridge tests and Runtime session regressions. `native_service_extension` builds on Windows, macOS and Linux. Run the [Native Service Demo](../web/native-service/README.md) for real-host checks. The five-platform CI matrix runs the same contracts. See [validation results](VALIDATION.md) for platforms actually exercised for this checkout.
