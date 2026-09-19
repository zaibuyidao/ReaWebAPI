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

## Windows

| Method | Resolved value | Behavior |
| --- | --- | --- |
| `ReaWebOpen(path)` | Window ID, `number` | Opens a local HTML file. Relative paths are based on the calling HTML directory |
| `ReaWeb_Close()` | `boolean` | Requests close. Document teardown can reject outstanding Promises, including an unsettled close call |
| `ReaWeb_Focus()` | `boolean` | Focuses the current window |
| `ReaWeb_DevTools()` | `boolean` | Requests inspector UI. On macOS use Safari Develop to attach |
| `ReaWeb_SetTitle(title)` | `boolean` | 1–256 UTF-8 bytes, no NUL |
| `ReaWeb_SetDocked(docked)` | `boolean` | Returns the actual docked state, preserving the page |
| `ReaWeb_IsDocked()` | `boolean` | Reads docked state |
| `ReaWeb_SetKeyboardCapture(capture)` | `boolean` | Default `true`. Setting `false` allows REAPER's normal global shortcut policy |
| `ReaWeb_GetWindowState()` | `ReaWebWindowState` | `id`, `title`, `docked`, `visible`, `focused`, `keyboardCapture` |

Window placement and docking are persisted by entry file and instance slot. At most 32 ReaWebAPI windows can be open. Handles, subscriptions and pending requests remain document-local even when browser storage is shared.

## Events

`ReaWeb_On(name, callback)` resolves to an asynchronous unsubscribe function. The callback receives an initial snapshot, then coalesced updates. The returned unsubscribe function can be called repeatedly. Catch errors from asynchronous callbacks yourself.

| Event | Callback payload |
| --- | --- |
| `projectchange` | `{ projectEpoch, changeCount }` |
| `selectionchange` | `{ projectEpoch, revision, count }`, where count is selected tracks |
| `windowstatechange` | `ReaWebWindowState` |

Project/selection observation runs about every 100 ms. `projectEpoch` changes on a project switch or load. Refresh object-dependent UI when it changes. A document close clears its subscriptions. `ReaWeb_Subscribe`, `ReaWeb_Unsubscribe`, `__reawebHello` and `__reawebReceive` are bridge internals, not application APIs.

## Batches and continuous controls

`ReaWeb_Batch(calls, { undoLabel? })` accepts 1–32 `{ method, args }` entries and resolves to their results in order. All arguments are prevalidated before native execution. A non-empty Undo label is limited to 256 UTF-8 bytes. Writes execute synchronously with paired UI refresh protection and, when a label is supplied, one Undo block.

| Accepted method | `args` |
| --- | --- |
| `CountTracks`, `CountSelectedTracks` | `[0]` or `[null]` |
| `GetTrack`, `GetSelectedTrack` | `[0, index]` or `[null, index]` |
| `GetTrackName` | `[track]` |
| `GetMediaTrackInfo_Value` | `[track, key]` |
| `SetMediaTrackInfo_Value` | `[track, key, value]` |
| `SetTrackColor` | `[track, color]` |
| `GetAppVersion` | `[]` |

Batch track keys are `D_VOL`, `D_PAN`, `B_MUTE`, `I_SOLO`, `I_CUSTOMCOLOR`. Its project arguments support the current project only. Get handles before building a batch: one entry cannot refer to an earlier result. Native void results occupy `null` slots in the batch result array. An execution failure rejects with `BATCH_FAILED`, whose details include `completed`, `results`, `rolledBack: false` and sometimes `cleanupError`. Completed writes are not reverted.

`ReaWeb_SetTrackValueLatest(track, key, value)` supports `D_VOL`, `D_PAN`, `B_MUTE`, `I_SOLO`. It keeps one in-flight write and the latest waiting value per track/key. Replaced waiting values resolve to `{ applied: false, superseded: true }`, while executed values resolve to `{ applied, superseded: false }`. There are at most 128 active track/key slots. It does not create an Undo gesture or change ordinary setter behavior.

## Errors and limits

`ReaWeb_SetBufferSize(bytes)` sets the document's default fixed output-buffer capacity and returns the new size. Valid range: 4 KiB–16 MiB, default 64 KiB. REAPER `NeedBig` buffers grow automatically. Large native outputs still need to fit the result limit.

| Limit | Value |
| --- | --- |
| Bridge JSON request/result | 64 MiB |
| String or binary value | 16 MiB |
| Audio array | 1,048,576 double elements |
| Pending JavaScript calls | 256 |
| Batch size | 32 calls |
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
