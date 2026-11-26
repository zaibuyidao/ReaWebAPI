# External Clients

[简体中文](external-clients.zh-CN.md)

External Clients are an optional entry point in the ReaWebAPI native extension on Windows, macOS and Linux. ReaWebAPI owns services, monitors, streams and main-thread dispatcher. Existing WebView and Lua Backend interfaces remain available independently.

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

Authenticated calls to the retired methods `api.call` and `api.batch` return `UNKNOWN_METHOD`. For project queries, call `runtime.getProjectSummary` through `service.invoke`.

| Method | `params` | Result |
| --- | --- | --- |
| `auth.authenticate` | `{ token, protocolVersion: 1 }` | `{ authenticated: true, protocolVersion: 1 }` |
| `system.getInfo` | `{}` | Extension version, protocol version, REAPER version, project epoch |
| `system.getCapabilities` | `{}` | Methods, services, events, streams and limits |
| `service.invoke` | `{ service, method, payload? }` | Service result |
| `service.send` | `{ service, method, payload? }` | `{ accepted: true }` |
| `service.subscribe` | `{ service, event }` | `{ subscriptionId }` |
| `service.unsubscribe` | `{ subscriptionId }` | `true` |
| `events.subscribe` | `{ event }` | `{ subscriptionId }` |
| `events.unsubscribe` | `{ subscriptionId }` | `true` |
| `stream.open` | `{ name }` | `{ consumerId, endpoint, ticket, info }` |
| `stream.close` | `{ consumerId }` | `true` |

Parameter objects reject unknown fields. An omitted service payload is JSON `null`. Unknown or foreign subscription/consumer IDs return `INVALID_ARGUMENT`.

## Services and events

Services share the existing registry and ABI, 1 MiB payload/result limit, 30-second invoke timeout, input dispatch and cancellation. ABI 1 callbacks receive `window_id == 0` for External requests because these callers have no window. Invoke replies route to the owning session. Existing positive Window ID event targets remain WebView-only. Broadcast service events reach subscribed WebViews and External Sessions. Services that require a real window can reject External requests.

Native Events share the existing monitor sources, snapshots, `projectEpoch`, `revision`, `available` and coalescing/invalidation behavior:

`track-selection-invalidated`, `track-state-changed`, `transport-state-changed`, `open-projects-state-changed`, `marker-list-changed`, `region-list-changed`, `current-region-changed`, `loop-points-changed`, `time-selection-changed`.

```json
{"type":"event","subscriptionId":"<opaque>","event":"transport-state-changed","data":{"available":true,"projectEpoch":1,"revision":2}}
```

Use the nine names above for new subscriptions. Their previous names are also accepted for compatibility. Each subscription has its own ID and notifications use the name requested at subscription time.

Service events also include `service`. A service unload rejects pending invokes and emits `unloaded` before removing its subscriptions. For External `open-projects-state-changed` events, `activeProject` matches an ID in `projects[]` when a project is active. IDs are opaque and scoped to the connection. Project loads and reconnects invalidate prior IDs. Each session supports at most 256 subscriptions.

## Binary streams and lifecycle

`stream.open` attaches a consumer to an already registered producer. `endpoint` is the complete WebSocket URL, including the single-use `ticket`. Connect within ten seconds. The ticket is bound to the External Session and target stream and does not depend on a WebView Origin. Access tokens are not stream tickets.

Use the existing [Native Stream binary packet and acknowledgement protocol](native-streams.md). After consuming a packet, send binary byte `0x01`. The control connection never carries stream packets. WebView streams retain their existing Origin validation. The shared stream hub permits 64 attached consumers across all callers.

Disconnect, server disable, token regeneration and extension unload cancel the affected session's pending invokes, subscriptions, project IDs and consumers. They do not unregister shared services or stop producers used by other callers. REAPER state and native resource limits are shared across callers.

Control requests and responses are bounded to 64 MiB, with at most 128 pending requests and 256 queued outbound messages per connection. Each direction has a 64 MiB per-connection queue budget. Response accounting is conservative and may reject a heavily nested value before its serialized size reaches that limit. Slow outbound connections time out after 30 seconds. `system.getCapabilities` reports the principal native limits and current registrations.

## Demo

Open [the standalone Demo](../examples/integrations/external-client/README.md) in a browser. It covers authentication, discovery, project and transport queries through the runtime service, events and existing binary streams.
