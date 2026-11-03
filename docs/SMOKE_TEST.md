# REAPER host acceptance

## JSFX parameter metadata

Run `python tests/jsfx_metadata_reaper.py --reaper <executable> --extension <binary> --output <new-profile>` on Windows, macOS and Linux, then repeat with `--chain`. The disposable profile verifies track, take, input, master and monitoring FX. Cases include numeric sliders, single-option enums, hidden/sparse sliders, aliased parameters, duplicate and Unicode labels, and file-selection sliders. Check `enum` against loaded engine values, including REAPER's expanded single-option host range and the engine's omission of empty source labels. Modifying the source file after loading must not change the enum snapshot. SDK gestures, host changes and `getState()` must preserve metadata. `jsfx_enum` covers ABI/range mismatch, UTF-8, truncation and read limits. Existing `jsfx_webui` and `jsfx_chains` tests cover optional APIs and instance routing. On macOS, keep the desktop unlocked and allow the script to activate its REAPER process through System Events. Dismiss the disposable profile's initial audio-device selection prompt if it appears.

## JSFX DSP stream recovery

Run `python tests/jsfx_stream_recovery_reaper.py --reaper <executable> --extension <binary> --output <new-profile>` on an isolated desktop. The fixture verifies spectrum, waveform, audio and meter data after a factory preset reset, source recompilation through offline/online reload, and `Audio_Quit`/`Audio_Init`. It checks retained page identity, four native streams, editor close/reopen and explicit stream close. macOS uses the current user's CoreAudio settings in the disposable profile. The test track has its master send disabled.

Also verify recompilation from the JSFX editor, physical device disconnect/reconnect and project switching. Valid silent streams must stay connected. Session replacement must discard old data and retain listeners. Explicit close, editor deactivation, page cleanup, deleted FX and invalid projects must stop recovery. `jsfx_stream_recovery_js` covers delayed opens, cancellation, subscription ownership, suspended DSP and bounded retries without a REAPER instance.

## Keyboard routing

- On macOS, open a floating App with `window.setKeyboardCapture(true)`. Press unmodified Enter repeatedly and verify one trusted `keydown`/`keyup` pair per press without blur. Switch to REAPER and back using the title bar or window switcher, then type without clicking the page. Repeat docked, with capture disabled, and with arrows, Space, letters, Shift+Enter and Cmd+Enter. Capture-disabled routing must retain REAPER's existing behavior.
- On Linux, focus an ordinary App and switch between floating and docked repeatedly. Verify three trusted Enter keydown/keyup pairs, letters, Space and Shift+Enter after each transition. Check actual native key delivery as well as `document.hasFocus()`.
- On Windows, macOS and Linux, test associated JSFX with `dawKeyboard` omitted, `true` and `false`, in floating FX windows and docked/undocked FX chains. Focus a knob, range input or button and press Space. Enabled mode must use the current main-section binding. Temporarily bind Space to a non-transport action in an isolated profile to rule out hard-coded Play/Stop.
- Verify input, textarea, nested contenteditable and open/closed Shadow DOM editing. Space must remain in the page during editing and IME composition. Check modified Space, other shortcuts, held Space, modifier changes during a press, switching focus while held, editor close/reopen and reload. No duplicate host action or unmatched host key release is allowed. Ordinary Apps retain their keyboard capture semantics.
- `python tests/keyboard_reaper.py --reaper <executable> --extension <binary> --output <new-directory>` exercises real Windows/Linux key input and a custom Space binding in a disposable profile. Run on an isolated desktop/display. On an unlocked macOS desktop, build `macos_keyboard`, then run `python3 tests/macos_keyboard_reaper.py --extension <dylib> --probe <reaper_keyboard_test.dylib> --output <new-directory>`. Repeat with `--jsfx default`, `--jsfx true` and `--chain`. For `--jsfx false`, record the old binary with `--baseline`, then compare the new binary using `--reference <baseline-result.json>`. Use `--controls-only` and `--reference` to compare ordinary shortcuts, optionally adding `--capture-disabled`. Native event posting permission is required. JavaScript unit tests cover routing decisions only and do not establish native input acceptance.

## Mouse pointer

- On Windows, enable **Hide pointer while typing**, focus a ReaWebAPI text field, then move the pointer over Arrange View without clicking. Type, pause and move within Arrange View. The pointer must hide during typing, remain hidden while stationary and reappear with REAPER's native shape on movement. Repeat several typing/movement cycles without changing focus. Repeat with the system option off, default/sRGB WebView rendering, floating/docked windows, multiple WebViews, reload and close/reopen. Check text selection, native drags and CSS `cursor: none` separately.
- Build and run `windows_cursor` on an unlocked interactive desktop, then repeat with `--srgb` and `--vanish-off`. Each invocation creates a fresh browser environment because WebView2 caches the system setting. It temporarily sets the system option for the selected case and restores it on exit. Run UI tests serially.
- Run `python tests/windows_cursor_reaper.py --reaper <reaper.exe> --extension <binary> --output <new-directory>` for 20 typing/movement cycles in real Arrange View. Use `--baseline` with an affected binary to verify the stuck-hidden reproduction.
- On macOS and Linux, verify text input and pointer movement between a WebView and Arrange View, including docked/floating windows. These backends retain native pointer handling.

## Windows JSFX focus

Run `python tests/windows_jsfx_focus_reaper.py --reaper <reaper.exe> --extension <binary> --output <new-short-path>` on an unlocked Windows desktop. The isolated profile checks parameter readback, repeated focus transitions between WebView2 and REAPER, floating/chain transitions and editor close/reopen. The host must continue processing deferred commands after every focus change. The runner closes its test process on completion and writes `result.json`. This interactive test is excluded from `ctest`.

## External Clients

- Confirm the server is disabled by default and existing WebView, Mirror and Lua Backend tools work. Enable **Preferences → Plug-ins → ReaWebAPI → External Clients**, apply, and use the [standalone browser Demo](../examples/integrations/external-client/README.md) with the copied token.
- Verify calls, Batch, native events and services with a WebView open. Disable the server and regenerate its token. Existing WebViews must remain usable, old External connections must close, and a new connection must use the current token. Test an occupied port and unchanged color-profile preferences.
- Build `native_stream_extension`, then run `python tests/external_client_smoke.py --reaper <executable> --extension <binary> --stream-extension <test-binary> --output <new-directory>`. The script creates an isolated REAPER profile and checks native calls before opening a legacy WebView, shared state/events, services, binary stream delivery, disconnect cleanup and handle isolation. It writes `result.json` on success and retains `startup.log` and `process.log` for startup diagnostics. On Windows, the test profile prevents automatic addition of system VST3 paths.

## Native communication

- Run `examples/integrations/native-service/reawebapi-native-service.lua`. Confirm the Lua action returns, the window stays open, `runtime.getInfo` works and all native state events continue. Repeat dock, undock, close and reopen.
- Change selection from none to one to multiple tracks, then replace the second track while retaining the first. Reorder selected tracks. Observe `trackSelectionChanged` without a browser or Lua polling loop.
- Change track name/color/mute/solo/arm/TCP/MCP visibility and add/delete/reorder tracks. Exercise play/pause/stop/record/rate/repeat, project tabs/open/close/save-as/dirty, marker/region metadata, current region, loop points and time selection.
- Use two WebViews, unsubscribe one and verify the other continues. Unsubscribe both and verify native monitor reads stop. Close/reload with pending service calls and ensure no stale delivery.
- Build and install the optional `native_service_extension` in the isolated host. Check ping, send/changed, on/off, repeated disposal, missing method/service, pending timeout and unregister while pending. Re-register and subscribe again. An old handle must not reach the new registration.
- Run `examples/integrations/native-service/reawebapi-native-service-coexist.lua` and use Lua echo while native features remain active. Run the existing Lua Backend Demo and check bidirectional messages and window behavior.

Automated native tests exercise the same lifecycle and comparison contracts. See [Native Services](native-services.md). Validation records are maintained locally and are not included in the repository or release bundles.

Run against a disposable REAPER project on each target architecture. CI core tests use a mock REAPER host and do not establish real-host UI compatibility.

- Install one architecture-matching binary in the resource `UserPlugins` directory and restart REAPER. Verify `reaper.APIExists("ReaWeb_Open")`.
- Load `Example.lua`. Open a page from a path containing spaces, Chinese characters, `#` and `%`.
- Select a track named `Guitar 吉他 "A"`; click **Get selected track name**. Verify exact text and console output. Clear selection and check the empty state.
- Open three windows while transport plays. Move/resize, type into an HTML input, and use REAPER controls. Closing any window must not stop transport or close REAPER.
- On Windows, macOS and Linux, repeatedly resize a dark page in the top/bottom and side Dockers, then undock and drag every edge and corner. Check for full-page flashes, white strips, stale content and viewport lag. Repeat with light, transparent and translucent HTML/body backgrounds, theme changes, reload, hidden Docker tabs, mixed display scaling and embedded/floating DevTools. Keep an input value and a running animation to verify page state survives resizing and docking.
- From Inspector, retain a track handle, delete that track, then call `GetTrackName` with the old handle. Expect `STALE_HANDLE`. Repeat after switching project tabs.
- Reload the page with requests pending. Old responses must not settle new-page requests.
- Verify HTML `<title>`, dynamic `document.title`, title removal/blank fallback and Unicode titles in floating and docked windows. Close a docked window and reopen it, then repeat after restarting REAPER. Check the Docker tab label, the outer title bar of a single-tab floating Docker, the WebView caption and `getState().title`. Background title changes must preserve the selected tab, focus and Docker placement. `setTitle()` must override later page changes and survive reload/instance reuse. Untitled pages retain the directory-based fallback without exposing `instanceKey`. The mock-host Windows check is `python tests/plugin_smoke.py --dll <extension> --webview --titles`.
- Open Inspector and verify `console.log`. Follow the [DevTools checks](#devtools-acceptance) for platform-specific behavior.
- In the Demo, click **Log selected track** and check the printed name and Pan, including no selection. Change selection, rename a track, and adjust Pan from both REAPER and the Demo. Confirm readback logs, final drag values, Copy/Clear, and optional REAPER console output. Scroll through the log while new entries arrive and verify that it remains bounded to 200 entries.
- Open two HTML Apps with distinct `app.json.id` values. Verify `reaweb://<appId>` origins, shared `ReaWebAPI/WebViewData/`, separate localStorage/IndexedDB and private `origin.json`, `Data/` and `WindowState/`. Restart REAPER, then move one App with its old root removed and verify identity and browser storage persist. Repeat with the old root retained but its recorded entry removed, leaving another HTML file intact. A second valid installation with the same ID must fail with `APP_ID_CONFLICT`. Without a manifest ID, verify `.reawebapi/identity.json` supplies a persistent random ID independent of launcher spelling. Move the marker with the App, then copy the complete App and verify separate origins and storage when the original remains valid. Production Apps must not create TCP listeners or save ports in `origin.json`.
- Verify schema 2 records with and without `entry`. Missing old roots recover in both cases. Existing legacy roots recover only when `app.json.entry` identifies an invalid original entry. Otherwise preserve the conflict. Check path variants, inaccessible paths, malformed records, symlink/junction escapes, concurrent opens and active/initializing windows. Failed checks must leave the record intact. Rebinding must preserve extra metadata, `Data/`, `WindowState/` and browser storage, and still work after restart.
- Check automatic identity with no manifest and with a manifest lacking `id`. Concurrent first opens of one root must share one marker, while complete copies with the same marker must bind distinct IDs when both entries remain valid. Deny writes, corrupt or lock the marker, and verify clear failures without regeneration. The marker and directory aliases must not be served as Web resources. Confirm old launcher-derived data remains untouched, explicit IDs take precedence, and JSFX still uses `fx_ident` without creating an ordinary marker. Run the existing launcher, single-instance and multiple-window checks unchanged.
- Close a window immediately during browser initialization, reopen it, then exit REAPER with several windows open. Check for crashes and surviving application windows.
- On Windows test a machine without WebView2 Runtime. Lua receives an error/console diagnostic without crashing REAPER; retry after installing the runtime.
- Test a missing entry file, remote entry URL, unknown API, forged handle and oversized message. No arbitrary native function should execute or external document replace the entry page.
- On Windows, macOS and Linux, follow HTTP/HTTPS/mailto links and redirect with `location.href` / `location.assign()`. Each blocked external navigation must open once in the system handler and preserve page state and bridge calls. Check local `other.html`, same-origin paths and query changes stay blocked with a console hint for `reaper.window.open(path)`. Hash changes must preserve the bridge. `window.open()` and `target="_blank"` must not launch a handler or create a window. Verify a missing system handler reports a console warning without closing the App.

Inspector behavior is documented in [DevTools](devtools.md). Linux binaries built on Ubuntu 24.04 should also be checked on the intended distribution and display server.

- Run SDK/web-runtime/reawebapi-web-runtime.lua without building: modules, local JSON fetch, Canvas, timers, file objects and browser storage must pass. Reopen and restart REAPER to check persistence; copy the App to a different directory to check isolation.
- Drag text and real OS files into its drop area. Confirm the browser handles them without navigating away from the App.
- Keep the window visible for animation checks, then dock/undock and resize: viewport dimensions must follow the native window and frames must resume. Check optional Workers, network and GPU capabilities on the target environment.
- Exercise item/take selection, transport and FX events; large MIDI payloads; batched/managed Undo cleanup on errors, close and project switch; file/clipboard/external-link helpers; production and Vite HMR. Confirm Undo/redo on a disposable real project.
- On macOS verify the localhost entry against the actual REAPER bundle's ATS policy. On Linux verify the default renderer and consult the Web Runtime document if WSLg/DMA-BUF stalls frames.


## Resize painting

For resize painting, build and run `windows_resize`. It uses a real WebView2 controller with a disposable page and profile, checks dark/light and transparent backgrounds, verifies child pixels survive an explicit background erase, and exercises repeated native reparenting and docked/floating viewport changes. It also checks ancestor `WM_SETREDRAW` batching, shared Docker hosts, hidden pages and restoration after undocking or destruction. It is excluded from `ctest`. In real REAPER, drag the Docker divider continuously rather than only resizing the WebView child programmatically. REAPER's ancestor redraw batching is part of this acceptance.

On macOS, build `macos_resize` and install `reaper_resize_test.dylib` alongside the extension in an isolated REAPER profile. Create `resize-test.enabled` containing the expected Docker position (`0` bottom, `1` left, `2` top, `3` right, `4` floating). Set `[reaper]` `dockermode0` to the same value for embedded modes or `32771` for floating, with `dockcompactsingle=1`. Set `dockheight`, `dockheight_t`, `dockheight_l` and `dockheight_r` to at least `300`. Ad-hoc sign the modules if required and launch with `-newinst -cfgfile <profile>/reaper.ini`. The test drives REAPER Docker dividers with native mouse events, verifies actual size changes, and captures the visible WKWebView surface. It also checks floating resizing, page content, animation progress, viewport/state retention, docking and theme changes. It writes `resize-test.log` and selected PNGs, then exits. Use an unlocked, idle graphical session with native window-capture permission. The test moves and restores the pointer. This opt-in target is excluded from `ctest`.

On Linux, build `linux_resize` and run `tests/linux_resize.py` on a dedicated X11 display with PyGObject and XTest available. Set `REAWEB_TEST_REAPER` to the REAPER executable, `REAWEB_TEST_BUILD_DIR` to the build directory (default `build-linux`) and `REAWEB_TEST_ISOLATED_DISPLAY=1`. The runner creates disposable profiles and drags all four Docker dividers. It checks actual size changes, sampled page pixels, visible content, viewport dimensions and retained input/animation state. Logs and PNGs are written under `.cache/linux-divider-*`. On WSL, use `REAWEB_TEST_PROFILE_DIR` on the Linux filesystem to avoid mounted-filesystem latency. Do not run this input test on a shared desktop. `linux_webkit_smoke.py` additionally verifies native mapping when showing, hiding, parking and restoring the embedded view.

## Undocked window frames

- On Windows and macOS, open an App floating, dock it, close and reopen it while docked, then undock it. Repeat after restarting REAPER with the docked state saved. Check the title bar, border and native window controls, and repeat docking/undocking several times. Page state, resizing and keyboard input must remain usable.
- On Windows, build `windows_startup` and run `windows_startup 1`, `windows_startup 1 --dock` and `windows_startup 1 --undock-pending` serially on an unlocked desktop. These cases check native frame rendering after undocking, repeated docking transitions and first-frame masking when undocking during initialization, using dark, light, gradient and delayed-stylesheet pages. The target is excluded from `ctest`.

## Windows Docker titles

Build `windows_dock_titles` with CMake and copy `reaper_dock_title_test.dll` alongside the extension in a disposable REAPER resource directory's `UserPlugins`. Create `dock-title-test.enabled` in that resource directory. Set `[REAPER]` options `dockermode0=32771` and `dockcompactsingle=1`, use the default English UI, and start REAPER with `-newinst -cfgfile <resource>/reaper.ini`. The test creates its own HTML fixture, checks the actual outer Docker caption, exercises inactive-tab title updates, closes the single-tab Docker and reopens the WebView. It writes `dock-title-test.log` and exits the disposable REAPER instance. Run again to check persisted docking across host restarts. This test requires WebView2 and is excluded from `ctest`.

For Windows Docker icons, build `windows_dock_icons` and install `reaper_dock_icon_test.dll` instead of the title test in a disposable profile. Create `dock-icon-test.enabled` and use the same launch settings. The test checks startup hiding before any custom icon or docking, native small/large icon pixels, replacement while hidden, inactive tabs, two WebViews, close/reopen, favicon removal, container restoration, focus and bounds. Results go to `dock-icon-test.log`. Repeat with `dockermode0=0` to check embedded Docker state without changing REAPER's main icon. For visual inspection, create `startup-icon-test.pause` to inspect the hidden startup icon or `dock-icon-test.pause` to inspect the docked favicon, and remove the file to resume. Verify the startup title has no icon or reserved space, the floating Docker draws the icon after switching back from a native tab, and pin/close controls remain functional.

## macOS/Linux Docker titles and icons

Build `swell_dock_windows` and install `reaper_swell_dock_test.dylib` (macOS) or `reaper_swell_dock_test.so` (Linux) alongside the extension in a disposable profile. Linux also needs the matching WebKit helper. Ad-hoc sign locally built macOS modules if required. Create `swell-dock-test.enabled` containing `floating`, set `dockermode0=32771`, `dockcompactsingle=1`, `mixwnd_vis=0` and `verchk=0` in `[reaper]`, and launch with `-newinst -cfgfile <resource>/reaper.ini` using the English UI. Repeat with `embedded` and `dockermode0=0`. The test verifies the actual mode, HTML/dynamic/fallback/explicit titles, inactive tabs, native icon pixels, hidden replacement, two WebViews, close/reopen, container restoration, main-window icons, focus and bounds. On macOS it also verifies the page and Docker context menus, manifest-based Unicode App labels, docking checks and disabled hidden DevTools mode actions. It declines the initial macOS audio-device setup prompt, writes `swell-dock-test.log` and closes the test instance. It is excluded from `ctest`.

The Linux `linux_window_icons` target additionally checks exact restoration of the original icon and decoration properties, including icon removal, ownership transfer and native host destruction.

## Lua window instances

Run `python tests/single_instance_smoke.py --reaper <executable> --extension <binary> --output <new-directory>` against a disposable host. Verify repeated launcher actions, copied launchers, different scripts sharing one HTML entry, named windows, omitted keys, `multiple=true`, initialization reuse and close/reopen. Manually minimize or dock a keyed window, rerun its launcher and verify focus without losing page state. Instance keys are exact strings supplied by Lua.

## Lua message bridge

For the message bridge, run `examples/backends/lua-backend/reawebapi-lua-backend.lua`, select a track with quotes, control characters and Unicode in its name, and verify exact UI text and volume readback. Reload the page and confirm it requests fresh state. Run a copy of the launcher in another directory and confirm independent queues. Closing either window must stop only its Lua backend. Automated real-host checks: `python tests/message_bridge_smoke.py --reaper <executable> --extension <binary> --output <new-directory>` uses an isolated resource directory.

## DevTools acceptance

- Windows/Linux: press Ctrl+Shift+I in the WebView to open DevTools, then press it again with DevTools focused to hide the same window. Reopen and verify the Console session is retained and no additional inspector appears. Check held keys across focus changes, rapid toggles during opening, native close and two WebViews in the same profile. Hiding must return focus to the corresponding page. Repeated API open calls must keep DevTools visible. Page timers and REAPER calls must continue while the inspector has focus.
- Linux: test the shortcut inside the inspector, **Hide DevTools**, and floating-window close. Drag the divider, resize, and switch **Float DevTools** / **Dock right**. Console entries, the selected DOM node and page JS state must survive mode changes. Hidden DevTools must stay hidden when resizing or docking the host. Reopen the tool and restart REAPER to check saved mode/width. Test a small viewport and a second display.
- Windows 11 / WebView2: with no saved preferences, verify a right-hand Embedded panel and a 40% width ratio. Drag the 1px divider in the system window-frame color, resize the host, and switch **Float DevTools** / **Embed DevTools** from the page context menu. Verify only the other mode and **Open DevTools** or **Hide DevTools** appear. The mode entry must be disabled while hidden or still opening, and enable once visible. Embedded content must fill the panel with no title bar, window controls, draggable caption or surrounding gaps. Floating mode must restore the native title bar and controls without a second frame. Check floating position, size and maximized state across switches. Console entries, the selected DOM node and page JS state must survive mode switches and hide/show. Test shortcuts in both views and menu hide/show. The floating native close button must end the inspector session, with the next open creating one inspector. Hidden panels must remain hidden on resize and host dock/undock. Reopen the tool and restart REAPER to verify both saved modes and width. Old floating preferences must remain floating.
- Windows: check other applications' shortcuts, two WebViews, REAPER window stacking and focus after host dock/undock. Test small windows, mixed-DPI displays and minimize/restore. Floating DevTools must not steal focus or remain globally topmost. When native hosting is unavailable, check floating fallback, disabled **Embed DevTools**, `devtools.fallbackReason` and unchanged saved preferences. Identification/keyboard failures must appear in `devtools.lastError`.
- Windows: open Demo and `lua-backend/reawebapi-lua-backend.lua` together, dock both in the same Docker, open embedded DevTools in Demo, then click the Lua backend Docker tab. Repeat in both directions and with both inspectors open. Test floating host windows and Floating DevTools too, including two pages sharing one profile. REAPER, both pages and keyboard input must remain responsive without focus returning to an inactive instance. Close each page with its native X button, wait for lifecycle cleanup, and rerun its launcher. The window and Lua polling loop must exit, and relaunch must create a usable instance without an already-running script prompt.
- macOS: Option+Command+I must show/hide the same native Inspector from the page or Inspector, ignoring repeats and returning focus to the page on hide. Verify dynamic **Open DevTools** / **Hide DevTools** and **Float DevTools** / **Embed DevTools**, page context-menu cancellation, and the absence of native **Inspect Element**. Embedded must fill the right side without a window title bar, window controls or wrapper. Floating must use WebKit's native window. Check Console entries, selected DOM node/tab, page JS state and connection across hide/show and mode changes. Verify rapid toggles during opening, idempotent API opens, native close/reopen, two WebViews, host dock/undock and closing with Inspector open. Drag the divider, resize, and reopen the tool to check saved preferences. At 552, 440 and 320 content points wide, verify that **Embed DevTools** stays enabled, both panes follow the saved ratio, and hiding restores the full page width. Check `visible`, `mode`, `embeddedSupported`, `nativeToggleSupported` and fallback diagnostics. Missing Inspector presentation controls must retain Floating. Missing native controls must disable menu actions and reject the API with `DEVTOOLS_UNAVAILABLE`.

Optional native DevTools tests create UI windows and are excluded from CTest:

```sh
# Linux, under X11/XWayland (or xvfb-run)
cmake --build build --target linux_devtools
GDK_BACKEND=x11 build/tests/linux_devtools
# Windows, on an idle interactive desktop (sends keys to the test's DevTools window)
cmake --build build --config Release --target windows_devtools windows_context_menu
build/tests/Release/windows_devtools.exe
build/tests/Release/windows_devtools.exe --dpi-v1
build/tests/Release/windows_devtools.exe --focus-switching
build/tests/Release/windows_devtools.exe --focus-switching-separate-profiles
build/tests/Release/windows_devtools.exe --dpi-v1 --focus-switching-separate-profiles
build/tests/Release/windows_devtools.exe --dialog-navigation
build/tests/Release/windows_devtools.exe --dpi-v1 --dialog-navigation
build/tests/Release/windows_context_menu.exe
# macOS, in a logged-in graphical session
cmake --build build --target macos_devtools
build/tests/macos_devtools
```

## WebView color profile

- Open Preferences > Plug-ins > ReaWebAPI. Confirm the **ReaWebAPI settings** border fills the page and follows its edges when resizing. Only the **Use sRGB for WebView rendering on Windows** checkbox and bottom-right **Restore defaults** button appear inside it. On Windows, check Cancel, Apply/OK, persistence and restoring the unchecked default, including cancellation of a reset. Restart REAPER to apply each change. Open two Apps and a development WebView, close/reopen them, reload and dock/undock. Compare neutral and saturated CSS colors against native UI on standard, wide-gamut and HDR displays where available.
- On macOS and Linux, confirm the checkbox is disabled and unchecked. Native color management must remain unchanged.

```sh
cmake --build build --config Release --target windows_color_profile
build/tests/Release/windows_color_profile.exe
build/tests/Release/windows_color_profile.exe srgb
```

On macOS/Linux, build `swell_preferences`. In a separate REAPER test resource directory, install only `reaper_preferences_test.dylib` or `.so` into `UserPlugins/` and create `preferences-test.enabled`. Launch REAPER with `-newinst -cfgfile <test-resource>/reaper.ini`. The test checks native SWELL controls, writes `preferences-test.log` and exits.

## Runtime and audio acceptance

Run `python tests/aggregate_source_smoke.py --reaper <executable> --extension <binary> --output <new-directory>` on Windows, macOS and Linux. It uses an isolated REAPER profile and checks all four aggregate stream kinds, default/false equivalence, nested folders, receives, Mute/Solo, muted items, PCM overflow and cancellation, routing changes/cycles, playback time, track deletion and consumer cleanup. It also checks that stream operations preserve project state and Undo. Results are written to `result.json`.

Run `python tests/aggregate_meter_smoke.py --reaper <executable> --extension <binary> --output <new-directory>` for empty buses, folders and upstream tracks. It verifies uninterrupted `processedSeconds`, RMS-M/I and LUFS-M/S/I against a non-aggregate source, then checks resets and recovery after media insertion/removal, media edits, route mute/unmute, seek and playback restart. macOS accepts `--audio-config <reaper.ini>` for the test audio device. Results are written to `result.json`.

Optional navigation tests use real WebViews with a simulated system URL handler:

```sh
cmake --build build --config Release --target windows_navigation
build/tests/Release/windows_navigation.exe
# Linux, under X11/XWayland
cmake --build build --target linux_navigation
python3 tests/linux_navigation.py build/tests/linux_navigation
```

- Load SDK/runtime-demo/reawebapi-runtime-demo.lua. Confirm theme colors, selected-track name, diagnostics, floating resize and dock/undock; hiding/showing must affect only this App container. Docked geometry setters should report WINDOW_DOCKED.
- In Demo and Starter on each OS, verify the WebView context menu starts with **Dock <App> in Docker**, checked only when docked, followed by Reload, the two state-dependent DevTools actions, Open <App> Folder and Close <App>. Default browser entries, including Inspect, must be absent. Check the Windows title-bar entry too. Repeat after resizing and changing Docker tabs. Verify page state, focus, keyboard input, saved docking state and full client-area WebView bounds, with no toolbar gap or page reload.
- On all platforms, right-click each Docker App tab with two Apps docked. Verify the checked **Dock <App> in Docker** item, lifecycle-aware **Reload**, dynamic DevTools actions, **Open <App> Folder**, and **Close <App>** affect only that App. Test an App launched from `examples/main-demo/index.html` whose HTML title is `ReaGBA`, live title updates, explicit `setTitle()`, Unicode and ampersands, `app.json` and directory-name fallback, native Inspector close/reopen and repeated floating/embedded switches. Hidden mode actions must be disabled in both page and tab menus.
- Check right-click on page background, links, selected text and editable fields. Default browser actions must remain hidden. A page `contextmenu` handler using `preventDefault()` must keep its custom menu without opening the native menu or changing docking state.
- Set PNG, multi-resolution ICO and SVG window icons using relative App-root and absolute Unicode paths. Check transparency, repeated replacement, dock/undock and moving between displays with different scaling. Invalid/missing files must reject without replacing the current icon. Confirm Demo uses `logo.svg`. On macOS check the floating document proxy icon without changing REAPER’s application icon; on Linux verify the window-manager icon where supported. Docked tabs follow REAPER’s presentation.
- Open Demo with only its HTML favicon declaration from its production `reaweb://<appId>` origin on each platform. Confirm the native icon matches `logo.svg`, then repeat from an HTTP development origin. Change/remove the declaration, change `<base href>` in a page that permits it, and switch matching `media` queries. Check delayed responses cannot replace a newer icon, failed/CSP-blocked loads preserve it, and removing all eligible declarations restores the default. Both favicon and successful `setIcon()` state must survive Dock/Undock, native window recreation and reload. An explicit override must still take precedence after reload, including when its source file has been removed.
- On each platform, open a new page without a favicon and call `setIconVisible(false)` during page initialization. Check the default title-bar icon and its reserved space disappear as soon as the call resolves, without docking or resizing. Repeat with a favicon. Update the icon while hidden, hide/show the window, reload and Dock/Undock repeatedly, then call `setIconVisible(true)` and verify the current icon returns. Check `getState().iconVisible` throughout. On Linux, verify both icon properties and decoration hints, and record whether the window manager honors them. Keep the Windows title-bar Dock entry and caption controls available.
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
## Hidden startup

Run `python tests/hidden_start_smoke.py --reaper <executable> --extension <binary> --output <new-profile> --count 20` on Windows, macOS and Linux. Add `--hold-seconds 180` to exercise prolonged background loading. The isolated host checks Runtime readiness, Lua/JS messages, Show/Focus, instance reuse, saved Docker restoration, Hide, Reload and Close.

During the hold period, switch to another application. Verify that Show leaves it active and Focus returns to REAPER with the WebView focused.

On Windows, build `windows_startup` and run `windows_startup 20 --hidden` for framed/frameless hidden creation, first Show/Focus desktop pixels, repeated Hide/Show, visible/hidden resize and early-close races. The first Show must paint without a second visibility toggle or forced focus. Run the default, `--dock` and `--undock-pending` modes for normal visible startup and docking regression. Windows 10 and Windows 11 require separate host runs. Linux requires X11/XWayland, and input-driven resize tests require an isolated display.

Run `windows_startup 1 --maximized` for saved maximized placement during first presentation, early Focus, hidden Show/Focus, minimize/restore and repeated Dock/Undock, including undocking before the first frame. Show must preserve foreground focus. The `plugin_smoke.py --webview --starter` check covers instance reuse and verifies the maximized state before docking, after undocking and in saved WindowState.

On macOS, build `macos_startup` in `build-arm64`, then run `STARTUP_VARIANTS=4,6 STARTUP_RUNS=20 python3 tests/macos_startup.py`. It uses cold isolated profiles to check framed/frameless hidden creation, background JS, first Show pixels without focus, Focus, repeated Hide/Show, resize and Close before initialization. Variants `0,1,2,3,5` check normal visible startup. A logged-in graphical session and screen-capture permission are required.

## Native close ordering

Build and run `windows_close` on Windows. It observes native child destruction for visible, hidden, initializing, framed, frameless, fixed-size and maximized windows. The host must already be hidden when child teardown begins.

On macOS build `macos_close`, then run `python3 tests/native_close.py` in a logged-in graphical session. On Linux build `linux_close` and run the same script on a dedicated X11 display with `REAWEB_TEST_ISOLATED_DISPLAY=1` and `REAWEB_TEST_REAPER` set. `REAWEB_TEST_BUILD_DIR` overrides the build directory. These tests check host visibility before WKWebView removal or X11 child unmap/reparent events, including repeated framed and frameless closes.

Check that the existing close lifecycle callbacks finish before teardown and saved placement survives reopening. Record frameless closes at normal and maximized sizes to check for caption, border or empty-host flashes. Native event ordering does not replace compositor-specific visual acceptance.

## Transparent floating windows

Run `python tests/transparent_smoke.py --reaper <executable> --extension <binary> --output <new-profile> --count 20` for transparent state, hidden startup, Lua/JS Dock rejection, instance reuse, reload and close. Runtime tests separately cover saved Dock preference preservation, menu rejection, invalid options and initialization failures.

Use the transparent Demo against a changing native background. Check clear margins, half-alpha panels, rounded corners and shadows in desktop captures. Opaque CSS must remain opaque. Repeat with framed/frameless, hidden/visible and resizable/fixed windows. Transparent pixels must still receive mouse input. Confirm drag exclusions, controls, IME, file drag-and-drop, Inspector, DPI/display transitions and ordinary-window coexistence.

- Windows: build `windows_transparent`, then run `build/tests/Release/windows_transparent.exe` and repeat with `--srgb`. This native backend test samples the desktop in four framed/hidden combinations and checks rectangular hit testing. Run the host smoke separately in REAPER.
- macOS: build `macos_transparent`. Install its test module beside the extension in a dedicated REAPER resource directory, write `0` (frameless) or `1` (framed) to `transparent-test.enabled`, and launch that profile. Screen capture and input permissions are required. The test checks desktop Alpha against a changing background, transparent-region input, hidden startup, Dock rejection and Hide/Show/Reload, then writes `transparent-test.log` and exits.
- Linux: run `tests/linux_transparent.py` with `REAWEB_TEST_ISOLATED_DISPLAY=1` and `REAWEB_TEST_REAPER` on a dedicated composited X11 display. It checks actual 32-bit hosts, desktop Alpha and input in real REAPER. Stop the compositor and repeat with `REAWEB_TEST_NO_COMPOSITOR=1` to verify explicit rejection. Record the window manager, compositor, WebKitGTK and rendering backend. Native Wayland is unsupported.

Keep ordinary `hidden_start_smoke`, frameless native input and Linux Docker divider tests in the regression run. Pixel samples do not replace first-presentation recordings or hardware-specific GPU, DPI and multi-display acceptance.

## Startup diagnostics and subscription concurrency

On macOS and Linux, build the opt-in `swell_jsfx_controls` target. Load its test module in a disposable REAPER profile containing `jsfx-controls-test.enabled`. The module checks hidden-control queries, meter lookup bounds, re-hidden controls, toolbar/meter/overlay preservation, native restoration and pre-paint observer timing. It writes `jsfx-controls-test.log` and exits that REAPER instance.

For the shared idle pool, run `windows_startup 1 --pool` and repeat the existing startup modes with `--pooled` as the final argument. On macOS use `STARTUP_POOLED=1` with `macos_startup.py`. On Linux use `REAWEB_TEST_IDLE_WEBVIEWS=5` with `linux_webkit_smoke.py`. Check consumption, replenishment, depletion fallback, App isolation, hidden loading and focus. In Preferences, verify default 2, Cancel, Apply, Restore defaults, persistence and live changes to 0 and 32 without closing active Apps. `--idle-webviews 0` on the startup probes disables the pool for comparison.

The macOS hidden-start test dismisses the evaluation dialog through its enabled button and waits for stable main-window focus before opening the WebView. Keep the test desktop idle. `startup-focus.jsonl` records key-window transitions, including transient focus changes during hidden loading and first Show. Repeat with `STARTUP_POOLED=0 STARTUP_IDLE_WEBVIEWS=0` to check cold creation.

`ctest` covers bounded timing records and JSFX subscription ordering, early/late registration cleanup, visibility during initialization and old-runtime sizing. Windows also covers shared environment failures, controlled retries, window closure and callbacks after teardown.

Run `python tests/plugin_smoke.py --dll <Windows-extension> --webview --startup --resource-root <isolated-profile>` to sample idle preparation, first/new windows, identity reuse and fully closed reopen. `startup.json` separates native milestones and browser resource/data timings. Repeat with fresh profiles and after restarting with the same profile. Compare medians and slower samples under the same machine load.

Run `python tests/jsfx_startup_reaper.py --reaper <executable> --extension <binary> --effect <1175-directory> --output <new-directory>` to check 20 tracks, multiple instances on one track, cached reopen and simultaneous floating editors. `--sdk <jsfx-webui.mjs>` selects a baseline helper. Each sample checks parameter identity and records native and browser timings. The test copies its fixture into a disposable profile. If REAPER does not execute command-line scripts, run that profile's generated `launch.lua` through its Action List. Use the existing editor-size, focus, hidden-start and native startup tests for presentation regressions. UI acceptance runs serially on each desktop.

## Live window appearance and file moves

Run `python tests/window_appearance_smoke.py --reaper <executable> --extension <binary> --output <new-profile> --count 20` for repeated frame changes, retained document state and outer bounds, hidden startup, docking and corner restoration. Creation `frameless` selects the initial appearance. `window.setFrameless()` changes the current window.

- Windows: run `windows_frameless` for full-bleed edge pixels, all eight resize directions, fixed-size windows, framed/frameless corner regions, maximization and docking. Run `windows_context_menu` for all six shared menu actions and page cancellation.
- macOS: run `macos_frameless` in an isolated REAPER profile with both resizable modes. Check retained content and responder, floating layer clipping, docking restoration and menu contents. Framed windows use system corners.
- Linux: run `linux_corners` and `linux_context_menu` on X11/XWayland, then the live appearance smoke under a window manager. Confirm decoration changes preserve the mapped host and content. Framed windows use window-manager corners.
- On each desktop, check radius 0, 24 and 64, maximize/restore, repeated Dock/Undock and display scaling. Record any window-manager or GPU-specific behavior.

`host_features` checks relative/absolute Unicode move paths, overwrite protection, same-file no-op, missing directories, symlink rejection and files larger than 16 MiB. Set `REAWEB_TEST_MOVE_VOLUME` to an existing writable directory on another filesystem to exercise staged cross-volume moves.

### App and JSFX Space routing (v0.3.9.5)

- Test omitted, `true`, `false` and invalid `dawKeyboard` in ordinary App manifests. Test JSFX association defaults independently of the adjacent app.json. Both page types support `setDawKeyboard`. Verify per-window overrides, no JSON writes, and rereading changed configuration on reload.
- Test floating and docked windows with keyboard capture both enabled and disabled. Confirm closed Shadow DOM inputs remain editable when enabling a page whose JSON started disabled.
- Bind Space to a different REAPER main-section action. Non-editable buttons and sliders must invoke that binding once per press. Text inputs, textareas, contenteditable and open/closed Shadow DOM editors must receive Space without invoking the host action.
- Disable routing and compare against the initial behavior. Check held-key release, window isolation and reset after reload. JSFX JSON defaults and opt-out behavior must remain unchanged.
- `tests/keyboard_reaper.py` includes the Windows/Linux ordinary App matrix. On macOS, run `tests/macos_keyboard_reaper.py --extension <module> --probe <keyboard-probe> --output <new-profile> --app-routing` on an unlocked desktop.
