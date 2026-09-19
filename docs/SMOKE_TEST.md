# REAPER host acceptance

Run against a disposable REAPER project on each target architecture. CI core tests use a mock REAPER host and do not establish real-host UI compatibility.

- Install one architecture-matching binary in the resource `UserPlugins` directory and restart REAPER. Verify `reaper.APIExists("ReaWebOpen")`.
- Load `Example.lua`. Open a page from a path containing spaces, Chinese characters, `#` and `%`.
- Select a track named `Guitar 吉他 "A"`; click **Get selected track name**. Verify exact text and console output. Clear selection and check the empty state.
- Open three windows while transport plays. Move/resize, type into an HTML input, and use REAPER controls. Closing any window must not stop transport or close REAPER.
- From Inspector, retain a track handle, delete that track, then call `GetTrackName` with the old handle. Expect `STALE_HANDLE`. Repeat after switching project tabs.
- Reload the page with requests pending. Old responses must not settle new-page requests.
- Open Inspector and verify `console.log`. On macOS use Safari Develop; Windows/Linux support the demo button.
- Open another HTML tool with `ReaWebOpen`. Verify one shared browser environment/profile and persistent `WebViewData`; no app-local WebView2 UDF appears.
- Close a window immediately during browser initialization, reopen it, then exit REAPER with several windows open. Check for crashes and surviving application windows.
- On Windows test a machine without WebView2 Runtime. Lua receives an error/console diagnostic without crashing REAPER; retry after installing the runtime.
- Test a missing entry file, remote URL, unknown API, forged handle and oversized message. No arbitrary native function or external navigation should execute.

macOS filesystem placement and programmatic Inspector differences are documented in the README. Linux binaries built on Ubuntu 24.04 should also be checked on the intended distribution and display server.
