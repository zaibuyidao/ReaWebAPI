# EEL2 backend support

**English** | [简体中文](eel-backend-compatibility.zh-CN.md)

ReaWebAPI's 16 script-exposed host functions can be called from EEL2 as well as Lua. ReaWebAPI v0.3.9.14 fixes the `ReaWeb_Open` optional fourth-argument binding. Use Lua `false/true` or EEL2 `0/1`. Omitting the argument still means `false`. Native C/C++ callers retain the `const bool*` signature and use `nullptr` for omission.

## Calling conventions

REAPER 6.24+ supports direct extension calls. The legacy `extension_api("ReaWeb_Name", ...)` form permits an `APIExists` check before calling a missing extension. Direct unknown functions fail compilation. See the [official ReaScript reference](https://www.reaper.fm/sdk/reascript/reascripthelp.html#extension_api).

```eel
single = ReaWeb_Open(#path, #key, "", 0);
extra = ReaWeb_Open(#path, #key, "", 1);
ReaWeb_IsReady(single) ? (
  ReaWeb_Send(single, "hello");
  ReaWeb_Receive(#message, single);
);
ReaWeb_GetLastError(#error);
```

| EEL2 call | Contract |
| --- | --- |
| `ReaWeb_Open(#path, optional #key, optional #id, optional multiple)` | Window ID, or `0` on failure. `0` reuses `(key,id)`, `1` creates an independent window. No key always creates a new window |
| `ReaWeb_OpenEx(#path, #json)` | Same options as Lua. JSON booleans must be `true/false`, not `0/1` |
| `ReaWeb_OpenDev(#url)` | Trusted loopback HTTP development page |
| `ReaWeb_Close(id)` | Close window |
| `ReaWeb_IsOpen(id)`, `ReaWeb_IsReady(id)` | Numeric booleans |
| `ReaWeb_Show(id)`, `ReaWeb_Focus(id)` | Show or focus window |
| `ReaWeb_SetDocked(id, docked)`, `ReaWeb_IsDocked(id)` | Actual docked state. Successful undocking returns `0` |
| `ReaWeb_DevTools(id)` | Request native developer tools, subject to platform availability |
| `ReaWeb_Send(id, #text)` | Queue UTF-8 text after readiness |
| `ReaWeb_Receive(#text, id)` | First output string receives the next message, or empty text |
| `ReaWeb_SetFrame(id, #text)` | Replace `frame.lua` text. Empty text clears it |
| `ReaWeb_GetDiagnostics(#json, id)` | First output string receives diagnostics JSON |
| `ReaWeb_GetLastError(#error)` | First output string receives the last native error |

The `const char*` return convention adds an output string **before** the native arguments. Do not assign the numeric function result to a string or pass a buffer length. EEL2 booleans are numbers. Lua `nil` placeholders have no EEL2 equivalent: omit trailing arguments or use empty strings for optional key/id fields.

## Limits

- EEL2 has different string construction limits from Lua. On the tested REAPER versions, `str_setlen` builds up to 65,536 bytes, while native-returned 1 MiB messages can be received and forwarded intact. This is not a universal 64 KiB bridge limit. Keep application payloads small and honor the [host queue limits](host-api.md#lua-message-bridge).
- `ReaWeb_Receive` uses empty text for both an empty queue and an empty message. Use a nonempty application protocol. Raw NUL is unsupported.
- Use `defer` for polling, `ReaWeb_IsOpen` to stop after close, and `atexit` for cleanup. EEL2 does not add worker-thread or audio-thread execution.
- `frame.lua` remains the field name for EEL2 content too. Native-only service, stream and task C APIs are not automatically ReaScript functions.
- Platform WebView, docking and DevTools constraints are the same as for Lua. EEL2 provides no built-in Lua table/JSON facilities.

The SDK includes a [minimal request/reply example](../web/eel-backend/README.md) and a [bidirectional track mixer](../web/eel-mixer/README.md). Both use only EEL2, HTML, CSS and JavaScript, with `ReaWeb_SetFrame` state refresh, `0..4` gain and acknowledged, bounded slider writes. Frame state is replaceable. Execution acknowledgements remain on the message channel.

## Validation

Validated with v0.3.9.14 on Windows x64 / REAPER 7.83, macOS ARM64 / REAPER 7.82 and Linux x64 / REAPER 7.82. These results cover the scenarios below, not every possible parameter combination.

The opt-in isolated-host tests cover direct/legacy syntax, Lua controls, optional arguments and window reuse, all 16 script functions, UTF-8 messages, frame delivery, and example UI/lifecycle behavior. Use a fresh output directory:

```text
python tests/eel_api_compatibility.py --reaper <executable> --extension <native-library> --output <new-directory> --suite full --direct
python tests/eel_api_compatibility.py --reaper <executable> --extension <native-library> --output <new-directory> --suite strings
python tests/eel_backend_smoke.py --reaper <executable> --extension <native-library> --output <new-directory> --demo eel-mixer
```

Omit `--direct` to test `extension_api`. Omit `--demo` to test the minimal example. These tests require an interactive desktop. Lua test fixtures prepare disposable projects and observe results, while the shipped EEL2 code handles application messages.
