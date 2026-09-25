# Host API reference

[Runtime API 完整清单 / Complete inventory](runtime-api-inventory.md) · [TypeScript](../runtime/runtime-api.d.ts)

**English** | [简体中文](host-api.zh-CN.md) · [Developer guide](development.md)

These are ReaWebAPI host methods, separate from the [730 REAPER APIs](api-reference.md). JavaScript window methods target their current WebView document and do not accept a window ID. All JavaScript methods return Promises. Exact types are in `reaper.d.ts`.

## Startup and capabilities

`reaper.lifecycle.ready` waits for one handshake per document, with no fixed startup delay. Once resolved, awaiting it again does not contact REAPER. Initial display time also includes WebView/page loading and subsequent API calls. Start visible data queries before unrelated window setup, and run independent calls concurrently.

Small isolated requests and bounded replies use a fast path, while native API execution stays in the main-thread scheduler. Large messages and file I/O use the worker. Delivery still depends on REAPER's scheduling and WebView responsiveness.

| Member | Resolved value | Notes |
| --- | --- | --- |
| `reaper.lifecycle.ready` | Capabilities plus `windowId`, `projectEpoch` | Promise property, not a function. API calls also wait for it automatically |
| `reaper.system.getCapabilities()` | `ReaWebCapabilities` | Version, protocol, registered methods, available native APIs, events and limits |
| `reaper.debug.getDiagnostics()` | `ReaWebDiagnostics` | Backend/browser version, window state, lifecycle stage, document/project generation, queue counters, last error and scheduler budget |

`capabilities.api` contains `schemaVersion`, `reaperVersion`, `catalogueHash`, `official`, `implemented`, `compatible`, `partial`, `missing`, `available`, `availableMethods`, `unavailable` and per-function `bindings`. `methods` includes registered host methods as well as standard APIs. A method being registered does not guarantee that its native function exists in an older REAPER. Check `api.availableMethods` for that.

`projectScope` is `all`. `limits` reports request bytes, batch calls and pending calls. Diagnostics counters describe this window, and `lastError` is a diagnostic string rather than a complete history of rejected calls. The scheduler's 2 ms budget is a soft dispatch budget, not a deadline for an individual native function.

Both capabilities and diagnostics include `webRuntime`: `contract` (1), `mode` (`app-http` / `dev-http`), `appId`, `origin`, `storageIsolation` (`origin`), and `localResources`. See the [Web Runtime contract](frontend.md).

## Windows

| Method | Resolved value | Behavior |
| --- | --- | --- |
| `reaper.window.openDev(url)` | Window ID | Trusted loopback HTTP development server; see [frontend resources](frontend.md) |
| `reaper.window.open(path)` | Window ID, `number` | Opens a local HTML file. Relative paths are based on the calling HTML directory |
| `reaper.window.close()` | `boolean` | Requests close. Document teardown can reject outstanding Promises, including an unsettled close call |
| `reaper.window.focus()` | `boolean` | Focuses the current window |
| `reaper.debug.openDevTools()` | `boolean` | Requests showing DevTools (idempotent). On macOS, unavailable native controls reject with `DEVTOOLS_UNAVAILABLE`. See [DevTools](devtools.md) |
| `reaper.window.setTitle(title)` | `boolean` | 1–256 UTF-8 bytes, no NUL. Overrides the [page title](runtime-api.md#window-titles) for this window, including reloads |
| `reaper.window.setIcon(path)` | `boolean` | Override the HTML favicon for this window session, including reloads. Local PNG, ICO or SVG, relative to the App root or absolute. See [window icons](runtime-api.md#window-icons) |
| `reaper.window.setIconVisible(visible)` | `boolean` | Show/hide the title-bar icon and its space, retaining the icon. Defaults to `true`, survives reloads. Linux decoration support depends on the window manager |
| `reaper.window.setDocked(docked)` | `boolean` | Returns the actual docked state, preserving the page |
| `reaper.window.isDocked()` | `boolean` | Reads docked state |
| `reaper.window.setKeyboardCapture(capture)` | `boolean` | Default `true`. Setting `false` allows REAPER's normal global shortcut policy |
| `reaper.window.getState()` | `ReaWebWindowState` | `id`, `title`, `docked`, `visible`, `focused`, `keyboardCapture`, `iconVisible` |

Window placement and docking are persisted by entry file and instance slot. At most 32 ReaWebAPI windows can be open. Handles, subscriptions and pending requests remain document-local while browser storage is shared only within the same App directory.

## Events

`reaper.events.on(name, callback)` resolves to an asynchronous unsubscribe function. State callbacks receive an initial snapshot, then coalesced updates. The `message` event delivers discrete Lua strings in FIFO order without snapshots or replay. The returned unsubscribe function can be called repeatedly. Catch errors from asynchronous callbacks yourself.

| Event | Callback payload |
| --- | --- |
| `projectchange` | `{ projectEpoch, changeCount }` |
| `selectionchange` | `{ projectEpoch, revision, count }`, where count is selected tracks |
| `itemselectionchange` | `{ projectEpoch, revision, count }`, selected items |
| `takeselectionchange` | Same shape, active takes of selected items |
| `transportchange` | `{ projectEpoch, available, state?, position?, cursor?, tempo? }` |
| `fxchange` | `{ projectEpoch, available, focused, touched, changeCount }` |
| `windowstatechange` | `ReaWebWindowState` |

Project changes and native track-selection notifications are observed on each main-thread tick. Selection retains a 100 ms fallback poll for edits without a notification. `projectEpoch` changes on a project switch or load. Refresh object-dependent UI when it changes. A document close clears its subscriptions. `ReaWeb_Subscribe`, `ReaWeb_Unsubscribe`, `__reawebHello` and `__reawebReceive` are bridge internals, not application APIs.

FX events observe focus, last-touched parameters and project changeCount to invalidate cached FX UI; they do not report every plugin parameter edit. Transport state is the REAPER bitmask; when available is false other transport fields may be absent. Selection scans are incremental, so large selections may take longer than 100 ms.

## Batches and continuous controls

`reaper.transaction.batch(callsOrBuilder, { undoLabel? })` accepts 1–128 calls, supplied as a call array or collected by a synchronous Builder callback. `capabilities.batchMethods` and `ReaWebBatchMethod` list 456 reviewed APIs for transport, project and object queries, MIDI, FX, envelopes, routing, markers, time mapping and conversion helpers. Project switching, action dispatch, modal dialogs, file I/O, resource lifecycle operations, manual Undo/refresh scopes and audio sample buffers are excluded. All 730 standard APIs remain callable individually.

Read transport values in one RPC without requiring a selected track:

```javascript
const data = await reaper.transaction.batch(b => {
  const position = b.GetPlayPosition();
  const state = b.GetPlayState();
  const tempo = b.Master_GetTempo();
  return { position, state, tempo };
});
```

Calls execute consecutively on the main thread. This reduces RPC round trips without freezing the audio clock or setting a polling rate. New methods still require availability in the installed REAPER version. Admission is reviewed per method, including all argument modes. Getters that can rescan files or instantiate resources are not admitted merely by name.

References can also connect dependent calls. This example requires a track at index zero:

```javascript
const data = await reaper.transaction.batch(b => {
  const track = b.GetTrack(0, 0);
  const [, name] = b.GetTrackName(track);
  const volume = b.GetMediaTrackInfo_Value(track, 'D_VOL');
  return { name, volume };
});
```

`b` preserves Mirror names, argument order and optional slots. Its methods return deferred references, with tuple indexing and destructuring in Lua result order. Return a reference or a nested plain object/array containing references and literals to select the resolved result. Omit the return value to receive the ordered native result array, as with `batch(calls)`. Void results remain `null` in both forms. `ReaWebBatchBuilder` derives method signatures from the Mirror, and `ReaWebBatchResult<T>` resolves the callback's result type.

The callback runs after the handshake and must be synchronous. References belong to that callback and can be passed as complete API arguments or returned, but cannot be awaited, used for arithmetic or inspected as real handles. They are always truthy objects, so do not branch on them or compare them to `null`. Read outside the batch when control flow depends on a native result. For example, check selection with `await reaper.GetSelectedTrack(0, 0)` before collecting calls that require a selected track. Unknown methods, async callbacks, cross-builder references and invalid structures reject with `INVALID_ARGUMENT` before dispatch. Callback exceptions propagate unchanged.

The Builder lowers calls to the existing `ReaWeb_Batch` request, with no additional RPC or Native layer. Native validation and `BATCH_FAILED` details remain unchanged.

`ValidatePtr` and `ValidatePtr2` can test null or expired object handles and return `false`. They do not conditionally skip later calls. Foreign-project handles are still rejected, and other object APIs still require live handles.

Batches operate on the **current project only**. Foreign project handles and objects are rejected before any write, including a foreign handle later in the batch. Methods, availability and arguments of calls without references are prevalidated. Arguments depending on earlier results are validated just before their call. Reference indices start at zero; optional paths select array elements or object properties, with at most eight segments. References must be complete top-level arguments and cannot point forward.

```javascript
const results = await reaper.transaction.batch([
  { method: 'GetSelectedTrack', args: [0, 0] },
  { method: 'GetMediaTrackInfo_Value', args: [{ $ref: 0 }, 'D_VOL'] },
  { method: 'SetMediaTrackInfo_Value', args: [{ $ref: 0 }, 'D_VOL', 0.5] }
], { undoLabel: 'Set selected track volume' });
// A null selection fails before its setter. Tuple results can use {$ref: 0, path: [1]}.
```

Each batch executes synchronously with paired refresh protection. An optional non-empty label (up to 256 UTF-8 bytes) creates one Undo block. Native void results occupy `null` slots. False/zero results retain their REAPER meaning; the legacy track-value setter rejects native false. A failed batch reports `BATCH_FAILED` with `completed`, `results`, `rolledBack: false` and optional `cause`/`cleanupError`. Completed writes remain; **this is not a transaction**.

`reaper.transaction.beginUndo(label)` returns a document-owned token; `reaper.transaction.endUndo(token)` closes it. Use `reaper.transaction.withUndo(label, async () => { ... })` for JavaScript finally cleanup. Managed gestures use the same reviewed API set and current-project restriction as batches. One gesture can be active across all windows; other windows' project calls, nested groups, batches and manual Undo/refresh calls reject `UNDO_BUSY`. No refresh lock is held between browser events.

The host closes a gesture on page reload/close, project switch/load or after 30 seconds. Ending an already closed token rejects `STALE_UNDO`. External scripts and user edits can interleave while awaiting; keep gestures short. This grouping cannot prevent external edits or roll back writes. Raw REAPER Undo scopes remain the caller's responsibility and are not covered by managed cleanup.

`reaper.audio.setTrackValueLatest(track, key, value)` coalesces waiting values for `D_VOL`, `D_PAN`, `B_MUTE` and `I_SOLO`. It retains one in-flight and one latest waiting write per track/key, up to 128 slots. Superseded waiting values resolve to `{ applied: false, superseded: true }`. Await all pending writes before ending a gesture.

## Files and desktop services

Files run on the worker thread; REAPER APIs remain on the main thread. These APIs use the user's filesystem permissions and **do not sandbox a trusted page to its folder**. Relative paths resolve from the calling HTML directory. Absolute paths and `..` are allowed. A Lua-opened development URL uses `<resource>/Scripts/` as its file base; a JavaScript-opened dev window inherits its caller's directory.

| Method | Result / options |
| --- | --- |
| `reaper.fs.readFile(path)` | UTF-8 string; invalid UTF-8 rejects `FILE_ENCODING` |
| `reaper.fs.readFile(path, { encoding: 'binary' })` | `Uint8Array` |
| `reaper.fs.writeFile(path, text, { overwrite?: boolean })` | `{ path, bytes }`; defaults to refusing existing files |
| `reaper.fs.writeFile(path, bytes, { encoding: 'binary', overwrite?: boolean })` | Binary write, `Uint8Array` input |
| `reaper.fs.stat(path)` | `{ path, exists, type, size }`; size is bytes for files, otherwise null |
| `reaper.fs.readDirectory(path)` | Sorted stat entries with `name`; maximum 4096 entries |
| `reaper.fs.makeDirectory(path, { recursive?: boolean })` | Whether a directory was created |
| `reaper.clipboard.readText()` | UTF-8 text, empty string if no text |
| `reaper.clipboard.writeText(text)` | Boolean |
| `reaper.system.openExternal(url)` | Boolean; sends http/https/mailto to the OS default handler |

File values and clipboard text are limited to 16 MiB. Clipboard writes reject NUL. Writes stage in the destination directory and commit by rename/link; parent directories must exist. `overwrite: true` explicitly replaces a file. Started I/O may finish after page closure; a lost reply does not imply the write failed. No delete, recursive removal or automatic retry is provided. Errors include `FILE_IO`, `FILE_NOT_FOUND`, `FILE_NOT_DIRECTORY`, `FILE_EXISTS`, `FILE_ENCODING`, `DIRECTORY_LIMIT`, `CLIPBOARD_BUSY`, `CLIPBOARD_ERROR`, `EXTERNAL_OPEN_FAILED`, `HOST_UNAVAILABLE`, `HOST_TIMEOUT` and `INVALID_URL`. External-open success means the OS accepted the request, not that a browser loaded the page.

## Errors and limits

`reaper.debug.setBufferSize(bytes)` sets the document's default fixed output-buffer capacity and returns the new size. Valid range: 4 KiB–16 MiB, default 64 KiB. REAPER `NeedBig` buffers grow automatically. Large native outputs still need to fit the result limit.

| Limit | Value |
| --- | --- |
| Bridge JSON request/result | 64 MiB |
| String or binary value | 16 MiB |
| Audio array | 1,048,576 double elements |
| Pending JavaScript calls | 256 |
| Batch size | 128 calls |
| Live handles per document | 65,536 |
| Queue expiry / client watchdog before execution | 25 s / 30 s |

Once native execution starts, its queue watchdog is cleared. Native functions cannot be preempted. No cancellation or automatic retry is provided. Objects deleted outside the page may invalidate handles before a queued call executes.

| Error code | Meaning and response |
| --- | --- |
| `NO_RUNTIME` | Missing native WebView transport. Open through the Lua launcher |
| `PROTOCOL_MISMATCH`, `SCHEMA_MISMATCH` | Incompatible bridge/definitions. Update the extension and reopen the page |
| `API_UNAVAILABLE`, `UNKNOWN_API` | Native function absent, or method not in this runtime. Check capabilities and API spelling |
| `INVALID_ARGUMENT`, `INVALID_REQUEST`, `INVALID_PATH` | Invalid values, message or local entry path |
| `INVALID_HANDLE`, `STALE_HANDLE` | Wrong handle type or expired object. Reacquire the object |
| `PROJECT_CHANGED`, `DOCUMENT_STALE`, `WINDOW_CLOSED` | Calling context changed. Refresh or reopen before continuing |
| `QUEUE_LIMIT`, `HANDLE_LIMIT`, `WINDOW_LIMIT` | Too many outstanding calls, handles or windows |
| `BUFFER_LIMIT`, `MESSAGE_LIMIT`, `ARRAY_CHANGED` | Oversized data, insufficient fixed buffer, or resized sample array |
| `TIMEOUT`, `REQUEST_EXPIRED` | No timely reply, or request expired before execution. Inspect state before repeating writes |
| `RESOURCE_IN_USE` | Attempt to destroy a PCM source still owned by a take |
| `BATCH_FAILED`, `UNDO_UNAVAILABLE` | Inspect completed results, or the host cannot provide the requested Undo grouping |
| `UNSUPPORTED_PARAMETER`, `UNSUPPORTED_PROJECT` | An argument is outside the batch prevalidator's narrower contract |
| `UNKNOWN_EVENT`, `DOCK_UNAVAILABLE`, `DOCK_FAILED` | Unsupported event or failed docking request |
| `ICON_FORMAT`, `ICON_INVALID`, `ICON_LIMIT` | Unsupported format, invalid image or icon limit exceeded |
| `ICON_APPLY_FAILED`, `ICON_SUPERSEDED` | Native icon application failed, or a newer request replaced this pending request |
| `NATIVE_ERROR`, `WRONG_THREAD` | Native failure or entry called off REAPER's main thread |

Error objects follow `ReaWebError`. Treat caught JavaScript values as unknown until checked. Native `false`/`0`/`null` returns remain ordinary results and must be interpreted using the corresponding REAPER API documentation.

## Lua entry points

These native extension functions are called synchronously from Lua. First use `reaper.APIExists("ReaWeb_Open")` to detect installation.

| Lua call | Return |
| --- | --- |
| `reaper.ReaWeb_OpenDev(url)` | Positive window ID, or 0 |
| `reaper.ReaWeb_Open(path, instanceKey?, id?, multiple?)` | Positive window ID, or `0` on failure |
| `reaper.ReaWeb_Close(id)` | Boolean |
| `reaper.ReaWeb_IsOpen(id)` | Boolean |
| `reaper.ReaWeb_IsReady(id)` | Boolean, true after document handshake |
| `reaper.ReaWeb_Send(id, message)` | `true` when queued for the ready document, otherwise `false` |
| `reaper.ReaWeb_Receive(id)` | Next UTF-8 text without blocking, or `""` for empty queue/error |
| `reaper.ReaWeb_Focus(id)` | Boolean |
| `reaper.ReaWeb_DevTools(id)` | Boolean |
| `reaper.ReaWeb_SetDocked(id, docked)` | Actual docked state |
| `reaper.ReaWeb_IsDocked(id)` | Boolean |
| `reaper.ReaWeb_GetDiagnostics(id)` | JSON string, empty on failure |
| `reaper.ReaWeb_GetLastError()` | Latest synchronous native entry error string |

Lua relative HTML paths resolve from REAPER's `Scripts/` directory. The starter uses its own absolute directory for portability. A positive window ID means the window was created, not that the page is ready. Loading errors can arrive later in the REAPER console and diagnostics. A Lua launcher does not need a defer loop to keep its page alive.

[v0.1.8 Runtime namespaces](runtime-api.md) · [v0.1.8 命名空间接口](runtime-api.zh-CN.md)

JavaScript opens pages with `reaper.window.open(path)` and waits on `reaper.lifecycle.ready`. Lua bootstrap uses `reaper.ReaWeb_Open(path)` because the browser has not started yet; the table above contains Lua-only native functions. The 730 standard REAPER mirror names are unchanged.

`reaper.events.off(name, callback)` removes all matching registrations of that callback. Subscribe to `native-drop` through `reaper.events.on('native-drop', callback)`; it carries files/text/x/y and has neither coalescing nor an initial snapshot. See [Runtime API](runtime-api.md).

### Window instances

`ReaWeb_Open(path, instanceKey?, id?, multiple?)` uses positional arguments, not a Lua table. Since v0.3.5, a nonempty `instanceKey` reuses and focuses the matching open or initializing window and returns its existing numeric window ID. Reuse preserves the page and ignores the new HTML path. Omitted, nil or empty keys create a new window on every call. `multiple = true` bypasses reuse without replacing an existing singleton.

```lua
local instanceKey = debug.getinfo(1, "S").source
local directory = instanceKey:sub(2):match("^(.*[/\\])")
local window = reaper.ReaWeb_Open(directory .. "index.html", instanceKey)
local settings = reaper.ReaWeb_Open(directory .. "settings.html", instanceKey, "Settings")
local extra = reaper.ReaWeb_Open(directory .. "index.html", instanceKey, nil, true)
```

Runtime compares `(instanceKey, id)` as separate, case-sensitive strings. Nil or empty `id` selects the default window. Keys are opaque: Runtime does not normalize paths, remove the `@` source prefix, or inspect the Lua stack. Capture the launcher source in the launcher itself and pass it through any helper. Use the same spelling on subsequent calls. Copied launchers in different directories have different keys. Closing or failed windows are excluded from reuse, and destroying the session releases its identity.

JavaScript `reaper.window.open(path)` and `ReaWeb_OpenDev(url)` retain their existing behavior. Native C/C++ consumers must use the v0.3.5 signature `int ReaWeb_Open(const char* path, const char* instanceKey, const char* id, const bool* multiple)` and pass `nullptr` for omitted options. Existing one-argument Lua calls remain valid.

## Lua message bridge

Two workflows coexist: pure JavaScript uses the REAPER Mirror and Runtime APIs. A Lua backend uses normal REAPER/ReaScript APIs and exchanges text with a WebView UI. See the [example](../web/lua-backend/README.md).

`ReaWeb_Send(id, message)` accepts UTF-8 text after `ReaWeb_IsReady(id)` is true. It does not serialize Lua tables or inspect JSON. The original string reaches `reaper.events.on("message", callback)`. Messages wait for a subscription in that document. Each subscribed callback sees subsequent messages without coalescing or replay.

`await reaper.host.send(message)` queues a string unchanged. Other JSON values are serialized with `JSON.stringify` before native dispatch. Unsupported values, cyclic objects, non-finite numbers and raw NUL text reject with `INVALID_ARGUMENT`. Success resolves to `true` when accepted, not when Lua processes it. Lua receives the serialized text with `ReaWeb_Receive(id)`.

Each window preserves FIFO independently in each direction. Limits are 1 MiB of UTF-8 per message, 256 pending messages per direction, and 16 MiB of queued payload across both directions, including native output awaiting dispatch. Oversized messages fail with `MESSAGE_LIMIT`, full queues with `QUEUE_LIMIT`. Native transport limits also apply. Limits are reported in `capabilities.runtime.host`.

Lua calls follow the existing `ReaWeb_GetLastError()` convention. Send fails before readiness (`NOT_READY`) or for a closed/unknown window (`WINDOW_CLOSED`). Receive returns `""` on empty queue or error. An empty string message is allowed and consumes its queue entry, but is indistinguishable from an empty queue in Lua. Use JSON text such as `null` when that distinction matters. Raw NUL is outside the text ABI. Successful calls do not clear previous errors.

Close and document navigation discard queued messages in both directions. No old-generation message is delivered to a replacement page. All Host API calls run on REAPER's main thread without waiting for a message. A Lua `defer` loop is needed only to poll continuously, not to keep a window open:

```lua
local function loop()
  if not reaper.ReaWeb_IsOpen(id) then return end
  for _ = 1, 32 do
    local text = reaper.ReaWeb_Receive(id)
    if text == "" then break end
    handle_message(text)
  end
  reaper.defer(loop)
end
loop()
```
