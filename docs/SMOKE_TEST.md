# REAPER host acceptance

Run against a disposable REAPER project on each target architecture. CI core tests use a mock REAPER host and do not establish real-host UI compatibility.

- Install one architecture-matching binary in the resource `UserPlugins` directory and restart REAPER. Verify `reaper.APIExists("ReaWebOpen")`.
- Load `Example.lua`. Open a page from a path containing spaces, Chinese characters, `#` and `%`.
- Select a track named `Guitar 吉他 "A"`; click **Get selected track name**. Verify exact text and console output. Clear selection and check the empty state.
- Open three windows while transport plays. Move/resize, type into an HTML input, and use REAPER controls. Closing any window must not stop transport or close REAPER.
- From Inspector, retain a track handle, delete that track, then call `GetTrackName` with the old handle. Expect `STALE_HANDLE`. Repeat after switching project tabs.
- Reload the page with requests pending. Old responses must not settle new-page requests.
- Open Inspector and verify `console.log`. On macOS use Safari Develop; Windows/Linux support the demo button.
- Open another HTML tool with `ReaWebOpen`. Verify that windows in one App directory share persistent storage and different directories have isolated localStorage, cookies and IndexedDB profiles.
- Close a window immediately during browser initialization, reopen it, then exit REAPER with several windows open. Check for crashes and surviving application windows.
- On Windows test a machine without WebView2 Runtime. Lua receives an error/console diagnostic without crashing REAPER; retry after installing the runtime.
- Test a missing entry file, remote URL, unknown API, forged handle and oversized message. No arbitrary native function or external navigation should execute.

macOS filesystem placement and programmatic Inspector differences are documented in the README. Linux binaries built on Ubuntu 24.04 should also be checked on the intended distribution and display server.

- Run SDK/web-runtime/Open.lua without building: modules, local JSON fetch, Canvas, timers, file objects and browser storage must pass. Reopen and restart REAPER to check persistence; copy the App to a different directory to check isolation.
- Drag text and real OS files into its drop area. Confirm the browser handles them without navigating away from the App.
- Keep the window visible for animation checks, then dock/undock and resize: viewport dimensions must follow the native window and frames must resume. Check optional Workers, network and GPU capabilities on the target environment.
- Exercise item/take selection, transport and FX events; large MIDI payloads; batched/managed Undo cleanup on errors, close and project switch; file/clipboard/external-link helpers; production and Vite HMR. Confirm Undo/redo on a disposable real project.
- On macOS verify the localhost entry against the actual REAPER bundle's ATS policy. On Linux verify the default renderer and consult the Web Runtime document if WSLg/DMA-BUF stalls frames.
