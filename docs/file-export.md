# Sequential file export

**English** | [简体中文](file-export.zh-CN.md)

`reaper.fs.openWrite()` exports files without constructing a whole-file bridge value. It is available to hosted Apps and JSFX WebUI, independent of App ID, isolation mode and External Client ports. Existing file methods retain their contracts and 16 MiB value limit.

```js
const writer = await reaper.fs.openWrite(destination, {
  overwrite: true,
  expectedBytes: blob.size,
});
try {
  for (let offset = 0; offset < blob.size; offset += writer.maxChunkBytes) {
    const bytes = new Uint8Array(await blob.slice(offset, offset + writer.maxChunkBytes).arrayBuffer());
    await writer.write(bytes);
  }
  const result = await writer.commit(); // {path, bytes}
} catch (error) {
  const state = await writer.abort();
  if (state.state !== 'committed') throw error;
}
```

| Operation | Contract |
| --- | --- |
| `fs.openWrite(path, {overwrite?, expectedBytes?})` | Returns a document-owned writer. Defaults to no overwrite. Relative paths use the entry directory. The parent must exist. URLs, directories and final-entry links are rejected. |
| `writer.write(Uint8Array, {offset?})` | Await each write. Offsets must equal the acknowledged byte count. Returns `{bytes, totalBytes}`. Input bytes are copied at invocation. No implicit retry. |
| `writer.commit()` | Flushes and publishes the complete file, returning `{path, bytes}`. Repeated successful commits return the same result. |
| `writer.abort()` | Cancels an unpublished writer and attempts cleanup. An already committed file remains committed. |
| `writer.status()` | Returns `{state, path, bytes, cleanupPending}`. States: `open`, `committed`, `aborted`, `failed`. |

Limits are advertised under `system.getCapabilities().runtime.fs.streamingWrite`: 1 MiB per chunk, 64 GiB per file, four active writers per Runtime and one per document. Calls use a separate bounded file executor. Await write/commit completion before starting another data operation. Idle writers expire after five minutes. Terminal status is retained for five minutes after its last operation, with at most 128 retained writers. Capacity exhaustion returns `WRITE_BUSY`.

Writes use an exclusive staging directory beside the destination. The existing destination remains unchanged until publication. Native write errors, abort, document navigation and window closure release unpublished writers and remove staging files. Failed cleanup is reported and retried while the Runtime remains alive. Commit requires the exact `expectedBytes`, when supplied. Publication uses a same-filesystem native rename, with a no-replace hard-link fallback on Linux. Filesystems that cannot perform the requested publish operation return an error. This does not guarantee survival across power loss. A process crash can leave a staging directory, and replacement does not preserve destination-specific permissions or metadata.

The publish operation is the cancellation boundary. If publication has already succeeded, abort reports `committed`. A lost reply alone does not prove failure. Query status or abort to resolve the result while the document remains alive. Handles cannot be transferred across windows or reloads. There is no resume, random write or crash recovery.

Common errors include `WRITE_BUSY`, `WRITER_INVALID`, `WRITER_CLOSED`, `WRITE_OFFSET_MISMATCH`, `WRITE_LENGTH_MISMATCH`, `BUFFER_LIMIT`, `FILE_EXISTS`, `ACCESS_DENIED`, `DISK_FULL` and `FILE_IO`.

## Browser helper

Existing three-argument `saveBlob(blob, filename, host?)` calls remain valid. The optional fourth argument adds cancellation and progress:

```js
import {saveBlob} from './browser-files.mjs';
const controller = new AbortController();
const path = await saveBlob(blob, 'capture.webm', reaper, {
  signal: controller.signal,
  onProgress: ({bytes, total, state}) => console.log(bytes, total, state),
});
```

The helper reads Blob slices of at most 256 KiB and waits for each native acknowledgement. Progress reports acknowledged bytes and `writing`, `committing` or `committed`. Cancelling the save dialog returns `null`. Aborting the signal rejects with `WRITE_CANCELLED` unless the native file has already committed. An unresolvable result returns `WRITE_RESULT_UNKNOWN`. The native save dialog itself cannot be dismissed by the signal.

The opt-in download handler streams connected `blob:` links and acquires their response before asynchronous capability/dialog calls. `data:` links and older hosts retain the 16 MiB fallback. Only one helper export per host is active, with `WRITE_BUSY` for concurrent attempts. Disposing cancels an unfinished export and removes the listener. Ordinary navigation and detached anchors remain unchanged.

Streaming export avoids an additional whole-file export buffer. It does not change how an App records video or constructs its Blob, and cannot make a recorder that retains all chunks use bounded memory.
