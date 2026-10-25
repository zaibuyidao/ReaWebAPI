# Checking that the frame timer works

[简体中文](frame-timer-verification.zh-CN.md)

This change moves one decision out of Lua. The extension now decides **when** the page refreshes. Lua still decides **what** the page shows.

Before, Lua sent a whole snapshot from its loop at most every 100 ms, and only when the content had changed. When Lua was slow the page stopped. Now Lua stores its content with `ReaWeb_SetFrame` and the extension pushes that content on its own cadence, together with four values it reads from REAPER itself: play state, play position, tempo and edit cursor. A slow loop then only delays new content; it no longer freezes the page.

Two things must both be true for this to work: the extension must be pushing frames, and Lua must be writing content. Check them separately.

## Install

Each platform installs a different set of files:

| Platform | What goes in `UserPlugins/` | Requirements |
| --- | --- | --- |
| Windows x64 | `reaper_reawebapi-x64.dll` | WebView2 runtime, VC++ x64 redistributable |
| macOS ARM64 / Intel | `reaper_reawebapi-arm64.dylib` or `reaper_reawebapi-x86_64.dylib` | WebKit from the system |
| Linux x86_64 / aarch64 | `reaper_reawebapi-<arch>.so` **and** `reawebapi-webview-<arch>` | WebKitGTK 4.1, X11 or XWayland |

On all three platforms, also copy the whole `Scripts/ReaWebAPI/Example/` folder.

On Linux, `reawebapi-webview-<arch>` is a separate executable. The extension looks for it in its own folder at startup and cannot open a window without it. If you copy the files by hand, give it execute permission:

```bash
chmod +x <resource folder>/UserPlugins/reawebapi-webview-*
```

Unpacking the platform ZIP from a release into the resource folder already places all of this, with the permission set. Open the resource folder with **Options → Show REAPER resource path in explorer/finder**.

Exit REAPER before copying, and start it again afterwards. The extension is loaded at startup, so a copy while REAPER is running has no effect.

## Step 1: does the extension have the new function

Create a ReaScript (Lua) in the Action List with a single line:

```lua
reaper.ShowConsoleMsg(tostring(reaper.APIExists("ReaWeb_SetFrame")) .. "\n")
```

`true` means Lua can hand content to the extension. `false` means the old extension is still installed, or REAPER was not restarted.

## Step 2: does the page move by itself

Run `Scripts/ReaWebAPI/Example/ReaWebAPI_Demo.lua`, press space in REAPER to start playback, and watch the readouts in the **Project snapshot** panel:

- **Playback** changes from `Stopped` to `Playing`, and the seconds after it change continuously. Not once a second, and not frozen.
- **Tempo** shows the current tempo. Change the tempo in REAPER during playback and this number follows immediately.

The three values behind these two rows — play state, play position and tempo — come from the extension reading REAPER directly, not through Lua, so they keep moving even when Lua has not run for a while.

Do not use the other two readouts to judge this:

- **Edit cursor** is also read by the extension, but it shows the edit cursor, not the play cursor. Playback does not necessarily move the edit cursor, so it staying still is normal.
- **Bar / beat** is not on this channel. It comes from the project snapshot Lua sends, and updates only when you select a track or read the project.

## Step 3: read the counters

Click **Backend diagnostics** to open the panel, click **Refresh snapshot**, then expand **View diagnostic JSON** to see the raw data:

```json
{
  "backend": {
    "backend": "Lua / reaper.defer + extension frame timer",
    "frameWrites": 42,
    "frameError": "",
    "stateSkipped": 18,
    "writeSkipped": 0,
    "received": 96,
    "sent": 138
  },
  "runtime": {
    "frames": {
      "pushed": 613,
      "intervalMs": 8,
      "luaWrites": 42,
      "luaFrameAgeMs": 12.3
    }
  }
}
```

`runtime` is the diagnostics the extension returns; `backend` is the counters Lua returns. Read them separately. `backend.frameWrites` and `frames.luaWrites` count the same thing — successful content writes from Lua — recorded independently by Lua and by the extension, so the two numbers should roughly match.

| Field | Meaning | What it looks like when working |
| --- | --- | --- |
| `frames.pushed` | Frames the extension has pushed | Rises from the moment the window is ready, **including while nothing happens and while the page has no subscription** |
| `frames.intervalMs` | The configured minimum push interval | Always `8`, not a measurement |
| `frames.luaWrites` | Successful `ReaWeb_SetFrame` calls | Rises when you select a track or move a fader; flat while idle, because unchanged content is not rewritten |
| `frames.luaFrameAgeMs` | How long ago Lua wrote the content on screen | Returns to near zero only when Lua writes new content. **While the page sits idle this number keeps growing, and tens of thousands is normal** |
| `backend.writeSkipped` | Drag steps replaced by a newer one before REAPER applied them | Rises while you drag a fader |
| `backend.stateSkipped` | Replies that left out the full snapshot, because a drag step does not need one | Rises while you drag a fader. It counts the drag steps that did reach REAPER, so it rises by more than `writeSkipped` |

Where the `frames.intervalMs` value of 8 comes from. The timer already paces this channel: REAPER calls it a median 31.7 ms apart, measured over 25 probe runs, so in normal running the page gets one frame per timer pass and this value never applies. It only applies to the pass REAPER runs immediately after a stall. That pass came 10.16 to 10.52 ms after a 130 to 141 ms stall, in all 25 runs. A floor of 16 ms sat above those passes and deleted them — 20 of 240 in each of three stall runs — and those are the first frames after a stall, which the page can draw. 8 ms is below every period measured, so it drops nothing the timer delivered, and it still bounds a burst faster than any run showed.

The panel shows two more things:

- `N frames drawn` at the end of the **REAPER access** row is how many frames the page received. The extension pushes frames before the page subscribes, so this is usually smaller than `pushed`.
- The **Lua messages** row shows five counters: `received`, `sent`, `frameWrites`, `writeSkipped`, `stateSkipped`.

How to read the numbers:

1. `pushed` rising: the extension is pushing frames. The core of this change works.
2. `pushed` rising but `luaWrites` never moving: the extension is pushing, Lua is not writing. Check the REAPER console for a Lua error.
3. `pushed` not rising at all: the extension is not pushing frames, so the old extension is still installed. Go back to step 1.
4. `pushed` and `luaWrites` both rising but the content on screen never updating: Lua wrote, the page did not receive it, so the page failed to subscribe to `frame`. `pushed` keeps rising in that case, so the counters alone cannot show it — look for `UNKNOWN_EVENT` or `SCHEMA_MISMATCH` in the error bar at the top of the page.
5. `luaFrameAgeMs` large while the page is idle: normal, ignore it. To test it, select a different track — it should drop back to within tens of milliseconds immediately.
6. `stage` stuck at `loading`: the window never finished its handshake.

## Difference from before

Before: the Lua loop was written to send a full snapshot at most every 100 ms, but the loop itself had to wait for REAPER to call `defer` — a median of about 31.7 ms between passes, and up to 111 ms when busy. The two delays stacked, giving a measured median send interval of about 126.8 ms. In between, the page did nothing at all.

After: the extension pushes frames on its own cadence whether or not Lua did anything. A slow Lua only delays new content.

The visible difference: start playback, then make REAPER busy — open a large project, or run a heavy action. Before, the play position in the page stopped moving. After, it keeps moving, and only the parts that need Lua, such as the track name, arrive late.

## When something is wrong

- Error bar at the top of the page: `ERROR: ...` means the page itself reported a failure.
- REAPER console (**View → Show console output**): Lua errors appear here.
- `runtime.recentLogs` inside `diagnostic-output`: recent extension-side log entries.

If all three are empty and the page still does not move, the installation is wrong. Go back to step 1.
