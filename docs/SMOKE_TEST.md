# REAPER host acceptance

Run against a disposable REAPER project on each target architecture. CI core tests use a mock REAPER host and do not establish real-host UI compatibility.

- Install one architecture-matching binary in the resource `UserPlugins` directory and restart REAPER. Verify `reaper.APIExists("ReaWeb_Open")`.
- Load `Example.lua`. Open a page from a path containing spaces, Chinese characters, `#` and `%`.
- Select a track named `Guitar 吉他 "A"`; click **Get selected track name**. Verify exact text and console output. Clear selection and check the empty state.
- Open three windows while transport plays. Move/resize, type into an HTML input, and use REAPER controls. Closing any window must not stop transport or close REAPER.
- From Inspector, retain a track handle, delete that track, then call `GetTrackName` with the old handle. Expect `STALE_HANDLE`. Repeat after switching project tabs.
- Reload the page with requests pending. Old responses must not settle new-page requests.
- Open Inspector and verify `console.log`. Follow the [DevTools checks](#devtools-acceptance) for platform-specific behavior.
- In the Demo, click **Log selected track** and check the printed name and Pan, including no selection. Change selection, rename a track, and adjust Pan from both REAPER and the Demo. Confirm readback logs, final drag values, Copy/Clear, and optional REAPER console output. Scroll through the log while new entries arrive and verify that it remains bounded to 200 entries.
- Open another HTML tool with `reaper.window.open`. Verify that windows in one App directory share persistent storage and different directories have isolated localStorage, cookies and IndexedDB profiles.
- Close a window immediately during browser initialization, reopen it, then exit REAPER with several windows open. Check for crashes and surviving application windows.
- On Windows test a machine without WebView2 Runtime. Lua receives an error/console diagnostic without crashing REAPER; retry after installing the runtime.
- Test a missing entry file, remote URL, unknown API, forged handle and oversized message. No arbitrary native function or external navigation should execute.

Inspector behavior is documented in [DevTools](devtools.md). Linux binaries built on Ubuntu 24.04 should also be checked on the intended distribution and display server.

- Run SDK/web-runtime/Open.lua without building: modules, local JSON fetch, Canvas, timers, file objects and browser storage must pass. Reopen and restart REAPER to check persistence; copy the App to a different directory to check isolation.
- Drag text and real OS files into its drop area. Confirm the browser handles them without navigating away from the App.
- Keep the window visible for animation checks, then dock/undock and resize: viewport dimensions must follow the native window and frames must resume. Check optional Workers, network and GPU capabilities on the target environment.
- Exercise item/take selection, transport and FX events; large MIDI payloads; batched/managed Undo cleanup on errors, close and project switch; file/clipboard/external-link helpers; production and Vite HMR. Confirm Undo/redo on a disposable real project.
- On macOS verify the localhost entry against the actual REAPER bundle's ATS policy. On Linux verify the default renderer and consult the Web Runtime document if WSLg/DMA-BUF stalls frames.


## DevTools acceptance

- Windows/Linux: press Ctrl+Shift+I in the WebView to open DevTools, then press it again with DevTools focused to hide the same window. Reopen and verify the Console session is retained and no additional inspector appears. Check held keys across focus changes, rapid toggles during opening, native close and two WebViews in the same profile. Hiding must return focus to the corresponding page. Repeated API open calls must keep DevTools visible. Page timers and REAPER calls must continue while the inspector has focus.
- Linux: test the shortcut inside the inspector, **Hide DevTools**, and floating-window close. Drag the divider, resize, and switch **Float DevTools** / **Dock right**. Console entries, the selected DOM node and page JS state must survive mode changes. Hidden DevTools must stay hidden when resizing or docking the host. Reopen the tool and restart REAPER to check saved mode/width. Test a small viewport and a second display.
- Windows: verify diagnostics report floating mode. Check that shortcuts in other applications are unaffected. Check stacking when REAPER or another REAPER dialog gains focus and after docking/undocking the WebView. A window opened through native **Inspect** may require its own close button. Check `devtools.lastError` when ReaWebAPI cannot identify it or cannot handle keys inside it.
- macOS: Ctrl+Shift+I must toggle the same non-modal Safari guide from either the page or the guide. Holding the keys must not repeat the toggle, and hiding must return focus to the page. The API must show the guide and report `INSPECTOR_MENU`. Follow the guide to inspect the page. Check docking/undocking and closing the tool with the guide open.

Optional native DevTools tests create UI windows and are excluded from CTest:

```sh
# Linux, under X11/XWayland (or xvfb-run)
cmake --build build --target linux_devtools
GDK_BACKEND=x11 build/tests/linux_devtools
# Windows, on an idle interactive desktop (sends keys to the test's DevTools window)
cmake --build build --config Release --target windows_devtools
build/tests/Release/windows_devtools.exe
```

## Runtime and audio acceptance

- Load SDK/runtime-demo/Open.lua. Confirm theme colors, selected-track name, diagnostics, floating resize and dock/undock; hiding/showing must affect only this App container. Docked geometry setters should report WINDOW_DOCKED.
- Use the native host’s **Dock/Undock** control in Demo and Starter, and the Windows title-bar **Dock in REAPER** menu. Repeat after resizing and changing Docker tabs. Verify page state, focus, keyboard input, saved docking state and WebView bounds, with no page reload or overlap with the native control.
- Set PNG, multi-resolution ICO and SVG window icons using relative App-root and absolute Unicode paths. Check transparency, repeated replacement, dock/undock and moving between displays with different scaling. Invalid/missing files must reject without replacing the current icon. Confirm Demo uses `logo.svg`. On macOS check the floating document proxy icon without changing REAPER’s application icon; on Linux verify the window-manager icon where supported. Docked tabs follow REAPER’s presentation.
- Open Demo with only its HTML favicon declaration. Change/remove the declaration, change `<base href>` in a page that permits it, and switch matching `media` queries. Check delayed responses cannot replace a newer icon, failed/CSP-blocked loads preserve it, and removing all eligible declarations restores the default. `setIcon()` must override automatic updates until reload, which restores the HTML favicon.
- Register asynchronous before-close/before-reload handlers that save a settings file, plus cleanup that terminates a Worker and destroys an owned audio accessor. Trigger the OS close button, Lua close API, SDK reload and browser reload. Reopen and inspect the saved file; no listener should run twice for one transition. A never-resolving callback must time out after approximately two seconds without blocking REAPER indefinitely.
- Change only the URL fragment with listeners registered. The page, handles and pending operations must remain alive, without a cleanup notification. Reload twice consecutively and verify that both reloads wait for cleanup.
- Add/delete/select tracks, edit Items/Takes, play/pause/stop, change tempo, edit markers/regions and FX parameters, load a project, save it, and use Save As. Verify the documented event payloads, no initial false added-track history, and cancellation on reload/close. Undo serialization alone must not report a saved project.
- Change REAPER's theme and verify CSS variables update. Dispose theme.apply and confirm prior inline colors return. Test JS uncaught errors, rejected Promises and invalid Native calls; inspect recentLogs for useful diagnostics.
- Use native dialogs to open Unicode paths, select a folder, cancel, and choose a save destination with filters. Choosing a path must not create the file until the App explicitly writes it.
- Open mono/stereo/multichannel WAV, FLAC and a compressed format supported by the installed REAPER. Compare sample rate, duration, channels and meaningful bit depth with REAPER. Draw a known test tone and silence from min/max waveform arrays; check channel ordering and a partial time range. Export JSON and compare it with the displayed data.
- Try an unsupported/corrupt file, an empty range and out-of-bounds options. No fabricated waveform or leaked PCM source should remain. Close/reload while a long waveform builds; the source and pending request must be cleaned up. Slow decoder calls may still occupy the main thread.
- During playback compare track meter channels with REAPER, including silence, multichannel tracks and a deleted/stale track handle. The API is a sampled peak reading, not a streaming or loudness analyzer.
- Validate a minimal manifest with the distributed tool. Reject a missing entry, absolute/parent-traversal path and a symlink escaping its App directory. Confirm an App without a manifest launches normally.

### App, system and native drag additions

- Check all five App getters with and without app.json; two windows from one entry directory must share ID/data, while another App directory gets its own data. Close/reopen, restart, rename and read-only data-directory cases should follow the documented identity/error rules. A writable probe runs at App creation; no probe file should remain.
- Runtime Studio shows metadata, platform and process architecture. Reveal a file and a folder in Explorer/Finder/the Linux file manager (including Linux ShowItems fallback).
- Choose audio first, then hold and drag its button into a disposable REAPER Arrange project. Confirm one native copy operation and Undo the imported Item; source files remain. Drag the App-name text to a native text editor. Escape and an incompatible target should resolve false.
- Drag standard native files/text from OS and REAPER into Studio, including Unicode paths and multiple files. Confirm files/text/x/y, no initial replay, and no duplicate notification after off/on or reload. REAPER proprietary drag formats are outside this contract.
- Use events.off with the same callback reference, duplicate registrations and an absent callback. Check debug.log and dragDrop callbacks.
- Optional automated native checks: build the Windows `windows_native_drag` target and run it from the build directory; on Linux run `WEBKIT_DISABLE_DMABUF_RENDERER=1 python3 tests/linux_native_drag.py` (PyGObject GTK 3, libXtst and X11 needed). These create temporary windows and restore the pointer afterward. They are deliberately excluded from unattended CTest. macOS requires real REAPER/AppKit testing.
