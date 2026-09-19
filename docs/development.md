# Developer guide

**English** | [简体中文](development.zh-CN.md) · [Documentation](README.md)

## Start a tool

Install the extension for your REAPER architecture, then copy the SDK directory to a writable development location. Load its `starter/Open.lua` in the Action List. This opens `index.html` next to the Lua file. The starter reads the selected track and provides a docking button.

```text
SDK/
  reaper.d.ts
  reaper-api.generated.d.ts
  runtime-api.d.ts
  starter/
    Open.lua
    index.html
    app.js
    style.css
    jsconfig.json
```

No web server, npm install or build step is needed for this JavaScript template. The Lua action can finish immediately after `reaper.ReaWeb_Open`: the extension owns the window. The page runs in a browser environment, not Node.js. Ordinary browsers do not provide `reaper.window.reaper`.

Use relative paths for bundled assets and a deferred classic script (`<script src="app.js" defer></script>`). If using a bundler, emit files suitable for a local HTML entry point. Test asset loading, module loading and any network requests on each target WebView. A development server URL cannot be passed to `reaper.window.open`, which accepts local `.html` or `.htm` files.

The starter's `jsconfig.json` enables JavaScript type checking without compilation. See the [TypeScript checkJs documentation](https://www.typescriptlang.org/tsconfig/checkJs.html). For TypeScript, include all three `.d.ts` files and compile your `.ts` files to browser JavaScript. The SDK declares a global object, so do not `import reaper`.

## Calling conventions

```javascript
await reaper.lifecycle.ready;
const track = await reaper.GetSelectedTrack(0, 0);
if (track) {
  const [ok, name] = await reaper.GetTrackName(track);
  if (ok) console.log(name);
}
```

Every method returns a Promise, including setters and void APIs. API calls automatically wait for the initial handshake, but awaiting `reaper.lifecycle.ready` gives one place to show startup failures. Use the generated [reference](api-reference.md) for the exact JavaScript signature and return labels, and its official links for units, flags, parameter keys and REAPER behavior.

| Native/Lua concept | JavaScript contract |
| --- | --- |
| One return value | A scalar after `await` |
| Multiple return values | An array in Lua order, including the native return value where present |
| No return values | `undefined` |
| Optional input | Omit trailing arguments, or use `null`/`undefined` for a positional hole |
| Current project | `0` or `null` in a `ReaProject` slot |
| Native object pointer | Typed opaque handle or `null`, as declared |
| GUID | Braced GUID string |
| RECT | Four coordinates in the Lua parameter/return order |
| MIDI/configuration bytes | `Uint8Array`, with binary results returned as `Uint8Array` |
| Audio sample array | `Float64Array` or `number[]`, updated before the Promise resolves |

Required inputs are not filled automatically. Boolean parameters require booleans. Integer inputs must fit their native type and JavaScript's safe range. Ordinary numeric inputs must be finite. Text inputs are UTF-8 and reject embedded NUL. Binary APIs preserve zero bytes and high bytes. `Float64Array` preserves IEEE 754 values, while ordinary `number[]` inputs require finite elements.

The catalogue covers standard C/Lua APIs, not Lua runtime helpers. Use DOM/canvas for UI and browser timers or host events for refresh. `reaper.defer`, `gfx`, Lua `require`, SWS and third-party extension methods are not supplied. Browser timers do not make REAPER API calls suitable for real-time audio processing.

## Availability, projects and handles

```javascript
const { api } = await reaper.system.getCapabilities();
if (!api.availableMethods.includes('GetTrackName')) {
  throw new Error('This REAPER installation does not provide GetTrackName');
}
const [project] = await reaper.EnumProjects(-1);
if (project) console.log(await reaper.CountTracks(project));
```

`implemented` describes the extension's bindings. `available` describes functions resolved in the running REAPER. The catalogue targets 7.80, and complete availability requires that version or newer. A known but unavailable function rejects with `API_UNAVAILABLE`.

Handles belong to one WebView document. Do not inspect their IDs, serialize them into settings, construct them from addresses or pass them between windows. Reacquire objects after deletion, document reload or `STALE_HANDLE`. Handles can refer to other open projects, but queued requests are rejected with `PROJECT_CHANGED` when the current project changes. Refresh UI state and let the user repeat a write deliberately.

## Writes and Undo

Use a batch for supported operations that should form one synchronous Undo step:

```javascript
const track = await reaper.GetSelectedTrack(0, 0);
if (track) {
  await reaper.transaction.batch([
    { method: 'SetMediaTrackInfo_Value', args: [track, 'D_PAN', 0] },
    { method: 'SetMediaTrackInfo_Value', args: [track, 'B_MUTE', 0] }
  ], { undoLabel: 'Center and unmute track' });
}
```

The [batch contract](host-api.md#batches-and-continuous-controls) covers 173 reviewed standard APIs, result references and up to 128 calls. All 730 methods remain available through ordinary calls. A batch is not a transaction: completed writes remain if a later call fails, and its error reports completed results.

For continuous controls across awaits, use managed Undo as described in the [host reference](host-api.md#batches-and-continuous-controls). It uses the reviewed batch API set and closes on reload, close, project change or after 30 seconds. Raw REAPER Undo scopes remain available outside this set, but callers must pair them and cannot rely on cleanup requests after a page closes.

For slider input, `reaper.audio.setTrackValueLatest` coalesces waiting values for a track/key. It does not create an Undo gesture. Await dependent calls in order. Use bounded concurrency for independent reads rather than enqueueing thousands of calls.

## Binary data and owned resources

```javascript
// Read MIDI bytes without converting them to text.
const item = await reaper.GetSelectedMediaItem(0, 0);
const take = item ? await reaper.GetActiveTake(item) : null;
if (take && await reaper.TakeIsMIDI(take)) {
  const [ok, bytes] = await reaper.MIDI_GetAllEvts(take);
  if (ok) console.log(bytes.byteLength);
}
```

```javascript
const track = await reaper.GetSelectedTrack(0, 0);
const accessor = track ? await reaper.CreateTrackAudioAccessor(track) : null;
if (accessor) {
  try {
    const channels = 2, frames = 256;
    const samples = new Float64Array(channels * frames);
    const start = await reaper.GetAudioAccessorStartTime(accessor);
    const status = await reaper.GetAudioAccessorSamples(accessor, 48000, channels, start, frames, samples);
    if (status > 0) console.log(samples[0]);
  } finally {
    await reaper.DestroyAudioAccessor(accessor);
  }
}
```

Do not resize, transfer or reuse an audio array while a call is pending. Read its contents after `await`. Allocate enough elements for channels, frames and the extra blocks required by peak APIs. See each API's official buffer layout.

Explicitly destroy created audio accessors, joysticks and unattached PCM sources when finished. Page closure/reload also releases resources still owned by that document. Assigning a created PCM source to a take transfers ownership to the project. Do not destroy a source still used by a take. See the corresponding native API before sharing or replacing resources.

## Events, UI and storage

```javascript
const stop = await reaper.events.on('selectionchange', state => {
  console.log('Selected tracks:', state.count);
});
// When this UI component is removed:
await stop();
```

Subscriptions deliver an initial snapshot and coalesced changes. Project changes and native track-selection notifications are checked on each main-thread tick, with a 100 ms selection fallback for silent edits. These are UI refresh signals, not an edit history or sample clock. Catch failures inside asynchronous callbacks. Dispose subscriptions when a component unmounts. Page closure clears the document's subscriptions.

Docking preserves page state. Browser profiles are isolated by App directory; windows in the same directory share storage. Use the [Web Runtime contract](frontend.md) for origin persistence and storage rules. Persist GUIDs or tool settings rather than object handles, and resolve persisted references against the appropriate project on the next run. Browser storage and network behavior follow the platform WebView. ReaWebAPI does not add Node.js filesystem or shell APIs.

## Errors and debugging

Bridge failures reject with `Error` plus `code` and optional `details`. Native API return values such as `false`, `0` or `null` retain the meaning documented by REAPER and do not automatically become exceptions.

```javascript
try {
  const track = await reaper.GetSelectedTrack(0, 0);
  if (track) console.log(await reaper.GetTrackName(track));
} catch (error) {
  console.error(error); // Includes code/details for bridge failures.
}
```

Use `await reaper.debug.getDiagnostics()` to inspect backend, stage, queue counts and the last host error. Use `reaper.debug.openDevTools()` on Windows/Linux. On macOS, enable Safari's developer features and inspect the REAPER page through its Develop menu. Reopen the page after changing assets. Reloading creates a new document and invalidates its old handles.

An unstarted native request expires after 25 seconds, with a 30-second client watchdog. Once execution starts, the queue timer stops. A native dialog or render cannot be interrupted by JavaScript. Never retry a timed-out write automatically. Check state first. Limits and error codes are listed in the [host reference](host-api.md#errors-and-limits).

## Distribute your tool

Ship the Lua launcher, HTML, built JavaScript, CSS and required assets, preserving relative paths. Rename the starter's action description and window title for your tool. The `.d.ts` files and editor configuration are only needed by developers. Users need the native ReaWebAPI extension, not Python or a compiler.

State your minimum REAPER and ReaWebAPI versions. Feature-detect required methods and explain missing dependencies in the UI. Test empty projects, missing selection, Unicode paths, deleted objects, project switches, docking, repeated open/close and each supported OS. Use the larger bundled Demo for bridge diagnostics. A passing simulated ABI test is not proof of all native side effects on real projects.

`ReaWebAPI-ReaPack-v<version>.zip` contains seven native files in `extension/`, `ReaWebAPI.ext` and license notices. Publish your tool's pages and Lua entry separately. The SDK ZIP contains development materials.

## Modern frontend and host I/O

See the [frontend contract](frontend.md) for Vite/TypeScript, loopback development, native local-resource fetch and Workers. The [host reference](host-api.md#files-and-desktop-services) covers common file, clipboard and external-link APIs.
