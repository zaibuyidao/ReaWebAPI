# Runtime Diagnostics

**English** | [简体中文](diagnostics.zh-CN.md)

Windows, macOS and Linux share a persistent diagnostic logger for Apps and JSFX WebUI. Configure it in **Preferences > Plug-ins > ReaWebAPI > Diagnostics**.

| Setting | Effect |
| --- | --- |
| Enable App debug logging + Target App ID | Enables INFO/DEBUG for the exact App ID and its associated Runtime events, across all its instances |
| Enable Runtime debug logging | Enables INFO/DEBUG for Runtime infrastructure, including events with no known App. Does not enable other Apps' JavaScript debug/info records |
| Open diagnostics log folder | Opens the log directory in the system file manager |

Both switches default to off and work independently. Apply/OK persists settings in `ReaWebAPI.ini` and updates active instances immediately. Configure the target before launching an App to capture initialization after its existing App ID becomes available. No App configuration changes are needed. The ID is the existing `reaper.app.getId()` / `webRuntime.appId`, including generated JSFX IDs. An empty target cannot enable App DEBUG.

Logs are UTF-8 JSON Lines under `<REAPER resource path>/ReaWebAPI/Diagnostics/`. Read them with any text editor. `runtime.log` contains Runtime events. `app-<appId>.log` contains the App's events, including associated Runtime events. Each file is limited to 2 MiB, with three numbered backups. The directory retains at most 64 managed log files, deleting the oldest when needed. Identifiers are preserved without filename truncation, restricted to existing lowercase ASCII App ID characters and a maximum of 240 characters for logging. Invalid diagnostic targets do not change App identity.

Every record has UTC `time`, `level` (`ERROR`, `WARN`, `INFO`, `DEBUG`), `scope`, `event`, diagnostic `sessionId`, `appId`, `windowId`, `result` and `threadId`. The diagnostic session identifies one Runtime lifetime and is independent of authentication and document tokens. Window IDs distinguish simultaneous instances. Known JSFX context adds `fxGuid`, `fxIndex`, `fxLocation`, `fxSessionId` and `activation`. API execution may include `method` and `durationMs`. Early events use an empty App ID and window ID 0.

Events cover creation, bridge readiness/navigation, closing and release, instance binding, API failures, JavaScript log severity, host-message channel activity and failures, lifecycle timeout, and JSFX identification, parking, attachment, invalidation and observed native-session changes. Track, input FX, Item/Take and nested Container FX use the existing resolver. Encoded FX indices retain the host's Container addressing.

Only WARN/ERROR are persisted by default. Free-form messages, URLs, arguments, message bodies, authentication tokens and audio samples are excluded, including text supplied to `reaper.debug.log/warn/error`. Existing in-memory logs and console output retain their current behavior. Repeated events are limited to 20 per second per App/window/event, with a `suppressed` count on the next emitted record. Writes flush on the UI thread. Calls from other threads are rejected before I/O. Diagnostic I/O failure is ignored by the Runtime.

REAPER does not expose a JSFX compilation callback. Native-channel session changes, host FX revision notifications, parameter-count changes and binding invalidation are observations, not proof of compilation. The Runtime also cannot observe an arbitrary Lua script's start or exit. `lua.channel.begin/end` describe observed use and release of the existing host-message channel, not Lua process lifetime. A stopped Lua polling loop without a window close has no definitive exit signal. A flushed record survives process termination, but this is not a power-loss durability guarantee.

The Demo's Diagnostics panel and `reaper.debug.getDiagnostics()` remain snapshots. Persistent logs require no viewer to stay open.

Active diagnostics require Runtime debug logging or App debug logging with a matching Target App ID. Runtime debug covers all instances, while App debug covers only the target App. These settings control probes, pending-request tracking, browser counters, host-gap checks and slow-completion checks. Disabling them stops the corresponding monitoring and clears its health snapshot. Existing passive WARN/ERROR records, including API failures and platform-reported WebView process failures, remain enabled.

Monitored, visible, ready Apps and JSFX WebUI share one probe every 5 seconds, with at most one outstanding evaluation. Hidden windows, parked JSFX, navigation and lifecycle cleanup suspend probing. Healthy reports are DEBUG only. Browser counters reset when monitoring is enabled or the document navigates. Requests already pending when monitoring starts are timed from that point and may have no method name.

| Event | Interpretation |
| --- | --- |
| `bridge.unresponsive` / `bridge.responsive` | No probe reply for 15 seconds / reply resumed. The renderer event loop or either bridge direction may be blocked. This does not prove a crash. |
| `bridge.slow` | Probe round-trip took at least 1 second. |
| `bridge.probe.fail` | The diagnostic evaluation could not be sent. This does not close the App. |
| `bridge.request.stalled` | The oldest browser request crossed 30 seconds without completion, even though the page can answer probes. Modal or asynchronous work may legitimately remain pending. Emitted once until a report falls below the threshold or monitoring resets. |
| `runtime.tick.delayed` | The host timer resumed after a gap of at least 2 seconds. Modal dialogs, sleep and host stalls can cause this. Pending probes are discarded to avoid attributing the gap to the renderer. |
| `api.slow` | At least 500 ms from native receipt to reply preparation, including queueing and asynchronous work, not solely execution time. |
| `javascript.health.warning` | Browser timeout, queue rejection, send/encoding failure, script/rejection/callback error or unexpected stream-close counters increased. |
| `webview.process.fail` | The platform reported a WebView process failure. |

Slow-probe, probe-send failure and slow-completion warnings share a 30-second per-window cooldown. Host tick warnings have a 30-second Runtime cooldown. Probe timeouts emit once until a reply or monitoring reset. Checks use the existing UI timer, with no audio-thread logging or additional watchdog thread.

Page reports include `pagePending`, `oldestAgeMs`, `oldestStarted`, oldest pending `method`, last `timeoutMethod`, cumulative error counters, `scriptLine`/`scriptColumn`, `activeStreams` and `streamPackets`. Native context includes `pendingCalls`, `queuedCalls`, `outputPending`, `hostMessages` and `webMessages`. Compare successive stream counters with transport state. Stopped audio or an idle producer may legitimately produce no packets. A started request may be waiting for a native dialog or asynchronous operation. `api.fail` includes the method, and service completions include `service`/`operation`. No arguments or stream payloads are captured. Snapshot `bridgeHealth` contains `enabled`, the last accepted page report and current probe flags, not a live sample. Querying it does not enable monitoring.

A permanently blocked REAPER UI thread cannot write new records or run probes. Rate-limited DEBUG API begin/end records may narrow the last native call but cannot replace a crash stack. Host gaps are observable only after the UI resumes. Hidden pages are not monitored for liveness, and script coordinates omit source URLs. Use a process dump or debugger when these observations do not identify the cause.
