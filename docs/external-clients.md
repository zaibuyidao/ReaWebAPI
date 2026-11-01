# External Clients

[简体中文](external-clients.zh-CN.md)

External Clients are an optional entry point in the ReaWebAPI native extension on Windows, macOS and Linux. ReaWebAPI owns the API core, services, monitors, streams and main-thread dispatcher. Existing WebView, Mirror and Lua Backend interfaces remain available independently.

## Configuration

In **Preferences → Plug-ins → ReaWebAPI → External Clients**, enable the server and apply the settings. The listener binds only `127.0.0.1`, defaults to port `9123`, and is disabled by default. Changes apply without restarting REAPER. A bind failure leaves the existing ReaWebAPI runtime available.

The first enable generates a 256-bit random access token. Settings use the `[ExternalClients]` section of `ReaWebAPI.ini` in the REAPER resource directory. **Copy** copies the token. **Regenerate** persists a new token and immediately disconnects all External Sessions. **Restore defaults** disables the server and restores port `9123` on Apply, retaining the token.

Connect to `ws://127.0.0.1:9123`. The token grants access to the exposed native operations and must be treated as a credential. Send it in `auth.authenticate`, never in the control URL. No WebView, Lua backend or JavaScript SDK is required. The server supports 16 concurrent connections and requires authentication within five seconds.

## Protocol version 1

```json
{"type":"request","id":1,"method":"auth.authenticate","params":{"token":"<access-token>","protocolVersion":1}}
```

```json
{"type":"response","id":1,"result":{"authenticated":true,"protocolVersion":1}}
```

Wait for the authentication response before sending other methods. Authentication failure or unsupported protocol version returns an error and closes the connection. Unauthenticated methods return `AUTH_REQUIRED`.

Requests use positive JavaScript-safe integer IDs, unique among pending requests on that connection. Responses may arrive out of order. Each accepted request receives one response while the connection remains open. Malformed JSON, invalid IDs, duplicate pending IDs and transport queue overflow close the connection. JSON nesting is limited to 64 levels. The control channel accepts text messages, including fragmented messages, and WebSocket ping/pong.

```json
{"type":"response","id":2,"error":{"code":"INVALID_ARGUMENT","message":"Wrong argument count"}}
```

Errors can include `details`, including existing `BATCH_FAILED` details. Native calls retain their existing wire values and error codes, except that foreign or forged handles use `INVALID_HANDLE` for External Sessions. Deleted objects retain `STALE_HANDLE`. Existing WebView handle errors are unchanged.

| Method | `params` | Result |
| --- | --- | --- |
| `auth.authenticate` | `{ token, protocolVersion: 1 }` | `{ authenticated: true, protocolVersion: 1 }` |
| `system.getInfo` | `{}` | Extension version, protocol version, REAPER version, project epoch |
| `system.getCapabilities` | `{}` | Methods, available APIs, batch catalogue, services, events, streams and limits |
| `api.call` | `{ name, args }` | Native API result |
| `api.batch` | `{ calls, undoLabel? }` | Ordered Native Batch results |
| `service.invoke` | `{ service, method, payload? }` | Service result |
| `service.send` | `{ service, method, payload? }` | `{ accepted: true }` |
| `service.subscribe` | `{ service, event }` | `{ subscriptionId }` |
| `service.unsubscribe` | `{ subscriptionId }` | `true` |
| `events.subscribe` | `{ event }` | `{ subscriptionId }` |
| `events.unsubscribe` | `{ subscriptionId }` | `true` |
| `stream.open` | `{ name }` | `{ consumerId, endpoint, ticket, info }` |
| `stream.close` | `{ consumerId }` | `true` |

Parameter objects reject unknown fields. An omitted service payload is JSON `null`. Unknown or foreign subscription/consumer IDs return `INVALID_ARGUMENT`.

## Native API and Batch

`api.call` accepts only implemented standard REAPER catalogue entries available in the running REAPER. It does not expose arbitrary `GetFunc` resolution, third-party APIs, Lua-only methods or JavaScript Runtime namespaces.

Arguments and results use the existing native JSON marshalling: handles are `{ "type": "MediaTrack", "id": "<opaque>" }`, multiple returns are ordered arrays, and binary/typed buffers use the existing tagged wire representation. Pass encoded values unchanged, including returned buffer writebacks. A handle belongs to one connection and expires on disconnect or relevant project invalidation.

Binary values use `{ "__reawebBytes": "<base64>" }`. Float64 inputs accept number arrays or `{ "__reawebFloat64": "<base64 little-endian IEEE-754 doubles>" }`. Calls with array writebacks return `{ "__reawebCall": true, "value": <native result>, "arrays": [{ "index": <argument index>, "values": { "__reawebBytes": "<updated doubles>" } }] }`. These encodings serve individual API buffers. Continuous data uses the binary Stream transport.

```json
{"type":"request","id":2,"method":"api.batch","params":{"calls":[{"method":"GetTrack","args":[0,0]},{"method":"GetTrackName","args":[{"$ref":0}]}],"undoLabel":"External operation"}}
```

Batch reuses the native allowlist, validation, 128-call limit, `$ref`/`path` references and partial-failure reporting. It does not imply rollback. Calls execute through ReaWebAPI's main-thread tick alongside existing windows. Queued API requests fail with `PROJECT_CHANGED` if the active project epoch changes. An active WebView managed Undo gesture blocks External API/Batch calls with `UNDO_BUSY`.

## Services and events

Services share the existing registry and ABI, 1 MiB payload/result limit, 30-second invoke timeout, input dispatch and cancellation. ABI 1 callbacks receive `window_id == 0` for External requests because these callers have no window. Invoke replies route to the owning session. Existing positive Window ID event targets remain WebView-only. Broadcast service events reach subscribed WebViews and External Sessions. Services that require a real window can reject External requests.

Native Events share the existing monitor sources, snapshots, `projectEpoch`, `revision`, `available` and coalescing/invalidation behavior:

`trackSelectionChanged`, `trackStateChanged`, `transportChanged`, `projectChanged`, `markersChanged`, `regionsChanged`, `currentRegionChanged`, `loopPointsChanged`, `timeSelectionChanged`.

```json
{"type":"event","subscriptionId":"<opaque>","event":"transportChanged","data":{"available":true,"projectEpoch":1,"revision":2}}
```

Service events also include `service`. A service unload rejects pending invokes and emits `unloaded` before removing its subscriptions. For External `projectChanged` events, project IDs are session-owned opaque handle IDs, never native pointer strings. Each session supports at most 256 subscriptions.

## Binary streams and lifecycle

`stream.open` attaches a consumer to an already registered producer. `endpoint` is the complete WebSocket URL, including the single-use `ticket`. Connect within ten seconds. The ticket is bound to the External Session and target stream and does not depend on a WebView Origin. Access tokens are not stream tickets.

Use the existing [Native Stream binary packet and acknowledgement protocol](native-streams.md). After consuming a packet, send binary byte `0x01`. The control connection never carries stream packets. WebView streams retain their existing Origin validation. The shared stream hub permits 64 attached consumers across all callers.

Disconnect, server disable, token regeneration and extension unload cancel the affected session's pending invokes, subscriptions, handles and consumers. They do not unregister shared services or stop producers used by other callers. API calls already executed are not rolled back. REAPER state and native resource limits are shared across callers.

Control requests and responses are bounded to 64 MiB, with at most 128 pending requests and 256 queued outbound messages per connection. Each direction has a 64 MiB per-connection queue budget. Response accounting is conservative and may reject a heavily nested value before its serialized size reaches that limit. Slow outbound connections time out after 30 seconds. `system.getCapabilities` reports the principal native limits and current registrations.

## Demo

Open [the standalone Demo](../examples/integrations/external-client/README.md) in a browser. It covers authentication, discovery, calls, Batch, Events, Service invocation and existing binary streams without a build step.
