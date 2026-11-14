# Browser files and media

ReaWebAPI v0.3.8.20 adds an opt-in SDK module, `browser-files.mjs`, for exporting browser-generated files through the existing native save dialog. Copy the module into your App and import it explicitly. It does not change navigation or install global download behavior in other Apps.

```js
import { saveBlob, installDownloadHandler } from './browser-files.mjs';

// Returns the selected path, or null on cancellation.
await saveBlob(new Blob(['example'], { type: 'text/plain' }), 'example.txt');

// For libraries that append and click an <a download> element.
const dispose = installDownloadHandler({ onError: error => console.error(error) });
addEventListener('pagehide', dispose, { once: true });
```

The handler intercepts connected `data:` and `blob:` download links. Ordinary navigation, remote downloads, detached anchors and `window.open()` are unchanged. Blob exports use sequential native writes when supported, with bounded 256 KiB helper chunks and a 64 GiB file limit. `data:` URLs and older hosts retain the 16 MiB fallback. Only one helper export per host is active. Concurrent attempts return `WRITE_BUSY`. Native errors propagate from `saveBlob`, or reach `onError` for intercepted links. Disposing cancels unfinished exports and removes the listener, but cannot dismiss an already open native save dialog. See [file export](file-export.md) for progress, cancellation and publication semantics.

## Browser-selected files

Use `<input type="file" multiple>` or DOM file drops to obtain browser `File` objects. Pass those objects directly to the player or create a Blob URL. Revoke URLs when their files are no longer in use. This path does not use `reaper.fs.readBinary` and is not subject to its 16 MiB binary-value limit. Browser memory, metadata parsing and codec limits still apply.

Windows uses WebView2's native file chooser. Linux uses WebKitGTK's [default file chooser](https://webkitgtk.org/reference/webkit2gtk/stable/class.FileChooserRequest.html). macOS implements [WKUIDelegate's open-panel callback](https://developer.apple.com/documentation/webkit/wkuidelegate/webview(_:runopenpanelwith:initiatedbyframe:completionhandler:)), including multiple selection and cancellation. WKOpenPanelParameters does not expose HTML `accept` filters to the host. Apps must validate selected file contents.

Web Audio, media codecs and WebGL2 require feature detection. Resume a suspended AudioContext from a user gesture. Close the audio context and release media resources when disposing a player. Blob URLs cannot be restored across documents. Persist a File copy in bounded browser storage, or ask users to select the original file again.

## Native media paths (v0.3.8.21)

Local Apps can open a native audio/video path without passing the file contents through the JSON bridge:

```js
const capabilities = await reaper.system.getCapabilities();
if (!capabilities.runtime?.fs?.mediaURLs) throw new Error('Media URLs unavailable');
const path = await reaper.dialog.openFile({ title: 'Open media' });
if (path) {
  const media = await reaper.fs.openMedia(path);
  const audio = document.querySelector('audio');
  audio.crossOrigin = 'anonymous'; // Also enables Web Audio with Linux's loopback media transport.
  audio.src = media.url;
  document.querySelector('#release-media').onclick = async () => {
    audio.pause();
    audio.removeAttribute('src');
    audio.load();
    await media.close();
  };
}
```

The frozen result contains `url`, `size` (bytes), `type` (MIME), and an idempotent async `close()`. Relative paths resolve against the entry document's directory. Paths must resolve to regular files. Supported extensions are WAV/WAVE, MP3, FLAC, OGG/OGA/OPUS, M4A/MP4, AAC, AIF/AIFF and WebM. The WebView's codec support determines actual playback support.

The opaque URL does not contain the native path. Windows and macOS use a `reaweb://` resource scoped to the creating window. Linux uses a random-port `http://127.0.0.1` media listener owned by that window, because WebKitGTK's GStreamer backend cannot decode custom URI schemes. Linux checks the exact loopback Host and owning App Origin (or same-App Referer); its random media URL is a bearer grant and must not be shared with other windows. Reload, navigation, close and explicit release revoke the grant, including outstanding readers. Missing or size/mtime-modified source files invalidate it at the next request/read. Save paths and reopen them in a new document, never persist these URLs. Already delivered browser buffers cannot be revoked.

Media responses support GET, HEAD and single byte ranges, including `206`, `Content-Range` and `416`. Multi-range requests and requests with an unvalidated `If-Range` use a full `200` response. Responses disable caching and resource sniffing. File reads are bounded to 64 KiB, with at most 32 open grants, 64 outstanding GET response readers per window and 64 GiB per file. A saturated reader limit returns `429`. Read failures terminate the response. There is no whole-file bridge copy. Linux's embedded listener serves only granted media, never static App files or arbitrary paths, and is independent of External Client settings. `reaper.fs.readBinary()` and its 16 MiB limit are unchanged.

The API inherits the existing trusted-local-App file-access model. It does not grant arbitrary web pages or External Clients local filesystem access. HTTP development pages are unsupported (`HOST_UNAVAILABLE`). A media grant does not make scripts, HTML or directories servable, or expand the static App root. Local App code remains trusted and can use other existing filesystem APIs.

Windows uses an IStream response, macOS asynchronously delivers scheme chunks, and Linux streams one chunk at a time through libsoup in the WebKit helper, discarding sent chunks. No media reads run on the REAPER audio thread. Codec support still depends on the installed browser and, on Linux, GStreamer codecs.

Allow local `connect-src`, `media-src blob:` and the player's required images, styles and Workers in the App CSP. Apps using native media on Linux must also allow `http://127.0.0.1:*` in `connect-src` and `media-src`. Set `crossOrigin = 'anonymous'` before assigning the media URL so requests identify the App origin and Web Audio receives the media signal. Some visualization presets compile expressions and require `script-src 'unsafe-eval'`. Scope that policy to the App that needs it. None of these browser features imply REAPER master-audio routing or Native Stream conversion.

The save helper is shared JavaScript for Windows, macOS and Linux. Windows build and regression coverage do not substitute for WKWebView or WebKitGTK acceptance.
