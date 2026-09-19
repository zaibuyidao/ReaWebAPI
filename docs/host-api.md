# Host API reference

**English** | [简体中文](host-api.zh-CN.md) · [Developer guide](development.md)

These are ReaWebAPI host methods, separate from the [730 REAPER APIs](api-reference.md). JavaScript window methods target their current WebView document and do not accept a window ID. All JavaScript methods return Promises. Exact types are in `reaper.d.ts`.

## Startup and capabilities

| Member | Resolved value | Notes |
| --- | --- | --- |
| `ready` | Capabilities plus `windowId`, `projectEpoch` | Promise property, not a function. API calls also wait for it automatically |
| `ReaWeb_GetCapabilities()` | `ReaWebCapabilities` | Version, protocol, registered methods, available native APIs, events and limits |
| `ReaWeb_GetDiagnostics()` | `ReaWebDiagnostics` | Backend/browser version, window state, lifecycle stage, document/project generation, queue counters, last error and scheduler budget |

`capabilities.api` contains `schemaVersion`, `reaperVersion`, `catalogueHash`, `official`, `implemented`, `compatible`, `partial`, `missing`, `available`, `availableMethods`, `unavailable` and per-function `bindings`. `methods` includes registered host methods as well as standard APIs. A method being registered does not guarantee that its native function exists in an older REAPER. Check `api.availableMethods` for that.

`projectScope` is `all`. `limits` reports request bytes, batch calls and pending calls. Diagnostics counters describe this window, and `lastError` is a diagnostic string rather than a complete history of rejected calls. The scheduler's 2 ms budget is a soft dispatch budget, not a deadline for an individual native function.

Both capabilities and diagnostics include `webRuntime`: `contract` (1), `mode` (`app-http` / `dev-http`), `appId`, `origin`, `storageIsolation` (`app-profile`), and `localResources`. See the [Web Runtime contract](frontend.md).

## Windows

| Method | Resolved value | Behavior |
| --- | --- | --- |
| `ReaWeb_OpenDev(url)` | Window ID | Trusted loopback HTTP development server; see [frontend resources](frontend.md) |
| `ReaWebOpen(path)` | Window ID, `number` | Opens a local HTML file. Relative paths are based on the calling HTML directory |
| `ReaWeb_Close()` | `boolean` | Requests close. Document teardown can reject outstanding Promises, including an unsettled close call |
| `ReaWeb_Focus()` | `boolean` | Focuses the current window |
| `ReaWeb_DevTools()` | `boolean` | Requests inspector UI. On macOS use Safari Develop to attach |
| `ReaWeb_SetTitle(title)` | `boolean` | 1–256 UTF-8 bytes, no NUL |
| `ReaWeb_SetDocked(docked)` | `boolean` | Returns the actual docked state, preserving the page |
| `ReaWeb_IsDocked()` | `boolean` | Reads docked state |
| `ReaWeb_SetKeyboardCapture(capture)` | `boolean` | Default `true`. Setting `false` allows REAPER's normal global shortcut policy |
| `ReaWeb_GetWindowState()` | `ReaWebWindowState` | `id`, `title`, `docked`, `visible`, `focused`, `keyboardCapture` |

Window placement and docking are persisted by entry file and instance slot. At most 32 ReaWebAPI windows can be open. Handles, subscriptions and pending requests remain document-local while browser storage is shared only within the same App directory.

## Events

`ReaWeb_On(name, callback)` resolves to an asynchronous unsubscribe function. The callback receives an initial snapshot, then coalesced updates. The returned unsubscribe function can be called repeatedly. Catch errors from asynchronous callbacks yourself.

| Event | Callback payload |
| --- | --- |
| `projectchange` | `{ projectEpoch, changeCount }` |
| `selectionchange` | `{ projectEpoch, revision, count }`, where count is selected tracks |
| `itemselectionchange` | `{ projectEpoch, revision, count }`, selected items |
| `takeselectionchange` | Same shape, active takes of selected items |
| `transportchange` | `{ projectEpoch, available, state?, position?, cursor?, tempo? }` |
| `fxchange` | `{ projectEpoch, available, focused, touched, changeCount }` |
| `windowstatechange` | `ReaWebWindowState` |

Project/selection observation runs about every 100 ms. `projectEpoch` changes on a project switch or load. Refresh object-dependent UI when it changes. A document close clears its subscriptions. `ReaWeb_Subscribe`, `ReaWeb_Unsubscribe`, `__reawebHello` and `__reawebReceive` are bridge internals, not application APIs.

FX events observe focus, last-touched parameters and project changeCount to invalidate cached FX UI; they do not report every plugin parameter edit. Transport state is the REAPER bitmask; when available is false other transport fields may be absent. Selection scans are incremental, so large selections may take longer than 100 ms.

## Batches and continuous controls

`ReaWeb_Batch(calls, { undoLabel? })` accepts 1–128 calls and returns results in order. `capabilities.batchMethods` and `ReaWebBatchMethod` list 173 reviewed APIs for tracks, items, takes, MIDI, FX, envelopes, sends, markers and tempo. Project switching, action dispatch, modal dialogs, file I/O, manual Undo/refresh scopes and audio sample buffers are excluded. All 730 standard APIs remain callable individually.

Batches operate on the **current project only**. Foreign project handles and objects are rejected before any write, including a foreign handle later in the batch. Methods, availability and arguments of calls without references are prevalidated. Arguments depending on earlier results are validated just before their call. Reference indices start at zero; optional paths select array elements or object properties, with at most eight segments. References must be complete top-level arguments and cannot point forward.

```javascript
const results = await reaper.ReaWeb_Batch([
  { method: 'GetSelectedTrack', args: [0, 0] },
  { method: 'GetMediaTrackInfo_Value', args: [{ $ref: 0 }, 'D_VOL'] },
  { method: 'SetMediaTrackInfo_Value', args: [{ $ref: 0 }, 'D_VOL', 0.5] }
], { undoLabel: 'Set selected track volume' });
// A null selection fails before its setter. Tuple results can use {$ref: 0, path: [1]}.
```

Each batch executes synchronously with paired refresh protection. An optional non-empty label (up to 256 UTF-8 bytes) creates one Undo block. Native void results occupy `null` slots. False/zero results retain their REAPER meaning; the legacy track-value setter rejects native false. A failed batch reports `BATCH_FAILED` with `completed`, `results`, `rolledBack: false` and optional `cause`/`cleanupError`. Completed writes remain; **this is not a transaction**.

`ReaWeb_BeginUndo(label)` returns a document-owned token; `ReaWeb_EndUndo(token)` closes it. Use `ReaWeb_WithUndo(label, async () => { ... })` for JavaScript finally cleanup. Managed gestures use the same reviewed API set and current-project restriction as batches. One gesture can be active across all windows; other windows' project calls, nested groups, batches and manual Undo/refresh calls reject `UNDO_BUSY`. No refresh lock is held between browser events.

The host closes a gesture on page reload/close, project switch/load or after 30 seconds. Ending an already closed token rejects `STALE_UNDO`. External scripts and user edits can interleave while awaiting; keep gestures short. This grouping cannot prevent external edits or roll back writes. Raw REAPER Undo scopes remain the caller's responsibility and are not covered by managed cleanup.

`ReaWeb_SetTrackValueLatest(track, key, value)` coalesces waiting values for `D_VOL`, `D_PAN`, `B_MUTE` and `I_SOLO`. It retains one in-flight and one latest waiting write per track/key, up to 128 slots. Superseded waiting values resolve to `{ applied: false, superseded: true }`. Await all pending writes before ending a gesture.

## Files and desktop services

Files run on the worker thread; REAPER APIs remain on the main thread. These APIs use the user's filesystem permissions and **do not sandbox a trusted page to its folder**. Relative paths resolve from the calling HTML directory. Absolute paths and `..` are allowed. A Lua-opened development URL uses `<resource>/Scripts/` as its file base; a JavaScript-opened dev window inherits its caller's directory.

| Method | Result / options |
| --- | --- |
| `ReaWeb_ReadFile(path)` | UTF-8 string; invalid UTF-8 rejects `FILE_ENCODING` |
| `ReaWeb_ReadFile(path, { encoding: 'binary' })` | `Uint8Array` |
| `ReaWeb_WriteFile(path, text, { overwrite?: boolean })` | `{ path, bytes }`; defaults to refusing existing files |
| `ReaWeb_WriteFile(path, bytes, { encoding: 'binary', overwrite?: boolean })` | Binary write, `Uint8Array` input |
| `ReaWeb_Stat(path)` | `{ path, exists, type, size }`; size is bytes for files, otherwise null |
| `ReaWeb_ReadDirectory(path)` | Sorted stat entries with `name`; maximum 4096 entries |
| `ReaWeb_MakeDirectory(path, { recursive?: boolean })` | Whether a directory was created |
| `ReaWeb_ClipboardReadText()` | UTF-8 text, empty string if no text |
| `ReaWeb_ClipboardWriteText(text)` | Boolean |
| `ReaWeb_OpenExternal(url)` | Boolean; sends http/https/mailto to the OS default handler |

File values and clipboard text are limited to 16 MiB. Clipboard writes reject NUL. Writes stage in the destination directory and commit by rename/link; parent directories must exist. `overwrite: true` explicitly replaces a file. Started I/O may finish after page closure; a lost reply does not imply the write failed. No delete, recursive removal or automatic retry is provided. Errors include `FILE_IO`, `FILE_NOT_FOUND`, `FILE_NOT_DIRECTORY`, `FILE_EXISTS`, `FILE_ENCODING`, `DIRECTORY_LIMIT`, `CLIPBOARD_BUSY`, `CLIPBOARD_ERROR`, `EXTERNAL_OPEN_FAILED`, `HOST_UNAVAILABLE`, `HOST_TIMEOUT` and `INVALID_URL`. External-open success means the OS accepted the request, not that a browser loaded the page.

## Errors and limits

`ReaWeb_SetBufferSize(bytes)` sets the document's default fixed output-buffer capacity and returns the new size. Valid range: 4 KiB–16 MiB, default 64 KiB. REAPER `NeedBig` buffers grow automatically. Large native outputs still need to fit the result limit.

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
| `NATIVE_ERROR`, `WRONG_THREAD` | Native failure or entry called off REAPER's main thread |

Error objects follow `ReaWebError`. Treat caught JavaScript values as unknown until checked. Native `false`/`0`/`null` returns remain ordinary results and must be interpreted using the corresponding REAPER API documentation.

## Lua entry points

These native extension functions are called synchronously from Lua. First use `reaper.APIExists("ReaWebOpen")` to detect installation.

| Lua call | Return |
| --- | --- |
| `reaper.ReaWeb_OpenDev(url)` | Positive window ID, or 0 |
| `reaper.ReaWebOpen(html_path)` | Positive window ID, or `0` on failure |
| `reaper.ReaWeb_Close(id)` | Boolean |
| `reaper.ReaWeb_IsOpen(id)` | Boolean |
| `reaper.ReaWeb_IsReady(id)` | Boolean, true after document handshake |
| `reaper.ReaWeb_Focus(id)` | Boolean |
| `reaper.ReaWeb_DevTools(id)` | Boolean |
| `reaper.ReaWeb_SetDocked(id, docked)` | Actual docked state |
| `reaper.ReaWeb_IsDocked(id)` | Boolean |
| `reaper.ReaWeb_GetDiagnostics(id)` | JSON string, empty on failure |
| `reaper.ReaWeb_GetLastError()` | Latest synchronous native entry error string |

Lua relative HTML paths resolve from REAPER's `Scripts/` directory. The starter uses its own absolute directory for portability. A positive window ID means the window was created, not that the page is ready. Loading errors can arrive later in the REAPER console and diagnostics. A Lua launcher does not need a defer loop to keep its page alive.
