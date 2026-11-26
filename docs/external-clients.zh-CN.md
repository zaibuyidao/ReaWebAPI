# External Clients

[English](external-clients.md)

External Client 是 ReaWebAPI 原生扩展中的可选接入功能，支持 Windows、macOS 和 Linux。Service、Monitor、Stream 和主线程调度仍由 ReaWebAPI 主体管理。现有 WebView 和 Lua Backend 接口可独立使用。

## 配置

在 **Preferences → Plug-ins → ReaWebAPI → External Clients** 中启用服务并应用设置。服务默认关闭，仅监听 `127.0.0.1`，默认端口为 `9123`。配置即时生效，无需重启 REAPER。端口启动失败不会阻止 ReaWebAPI 主体运行。

首次启用生成 256 位随机 Access Token，保存到 REAPER 资源目录 `ReaWebAPI.ini` 的 `[ExternalClients]` 节。**Copy** 复制 Token。**Regenerate** 立即保存新 Token 并断开全部 External Session。**Restore defaults** 在应用时关闭服务并恢复端口 `9123`，保留已有 Token。

连接地址为 `ws://127.0.0.1:9123`。Token 授予当前暴露原生操作的访问权限，应作为凭据保存。通过 `auth.authenticate` 发送 Token，不将其放入控制连接 URL。连接不需要 WebView、Lua Backend 或 JavaScript SDK。最多同时连接 16 个客户端，必须在五秒内认证。

## 协议版本 1

```json
{"type":"request","id":1,"method":"auth.authenticate","params":{"token":"<access-token>","protocolVersion":1}}
```

```json
{"type":"response","id":1,"result":{"authenticated":true,"protocolVersion":1}}
```

收到认证成功响应后，再发送其他方法。Token 错误或协议版本不支持时，返回错误并关闭连接。未认证的方法返回 `AUTH_REQUIRED`。

Request ID 使用正的 JavaScript 安全整数，在当前连接的待完成请求中保持唯一。响应可乱序返回，连接存续期间每个接受的请求对应一个响应。非法 JSON、非法 ID、重复待完成 ID 或传输队列超限会关闭连接。JSON 嵌套最多 64 层。控制通道支持文本分片和 WebSocket ping/pong。

```json
{"type":"response","id":2,"error":{"code":"INVALID_ARGUMENT","message":"Wrong argument count"}}
```

已认证客户端调用已下架的 `api.call` 或 `api.batch` 均返回 `UNKNOWN_METHOD`。读取工程信息请通过 `service.invoke` 调用 `runtime.getProjectSummary`。

| Method | `params` | 成功结果 |
| --- | --- | --- |
| `auth.authenticate` | `{ token, protocolVersion: 1 }` | `{ authenticated: true, protocolVersion: 1 }` |
| `system.getInfo` | `{}` | 扩展版本、协议版本、REAPER 版本、工程 epoch |
| `system.getCapabilities` | `{}` | 方法、Service、Event、Stream 和限制 |
| `service.invoke` | `{ service, method, payload? }` | Service 返回值 |
| `service.send` | `{ service, method, payload? }` | `{ accepted: true }` |
| `service.subscribe` | `{ service, event }` | `{ subscriptionId }` |
| `service.unsubscribe` | `{ subscriptionId }` | `true` |
| `events.subscribe` | `{ event }` | `{ subscriptionId }` |
| `events.unsubscribe` | `{ subscriptionId }` | `true` |
| `stream.open` | `{ name }` | `{ consumerId, endpoint, ticket, info }` |
| `stream.close` | `{ consumerId }` | `true` |

参数对象拒绝未知字段。省略 Service payload 时使用 JSON `null`。未知或其他 Session 的订阅、Consumer ID 返回 `INVALID_ARGUMENT`。

## Service 与 Event

Service 复用现有 Registry、ABI、1 MiB payload/result 限制、30 秒 invoke 超时、输入分发和取消规则。External 请求没有窗口，ABI 1 回调的 `window_id` 为 `0`。Invoke 响应返回所属 Session。正数 Window ID 定向事件仍只投递给对应 WebView，广播事件可到达订阅的 WebView 与 External Session。依赖真实窗口的 Service 可拒绝 External 请求。

Native Event 共用现有 Monitor source，保留 snapshot、`projectEpoch`、`revision`、`available`、合并和失效通知规则：

`track-selection-changed`、`track-state-changed`、`transport-state-changed`、`projects-state-changed`、`marker-list-changed`、`region-list-changed`、`current-region-changed`、`loop-points-changed`、`time-selection-changed`。

```json
{"type":"event","subscriptionId":"<opaque>","event":"transport-state-changed","data":{"available":true,"projectEpoch":1,"revision":2,"state":1,"playing":true,"paused":false,"recording":false,"position":8.5,"cursor":4,"rate":1,"loop":false,"tempo":120}}
```

新订阅推荐使用上述 9 个名称，旧名称仍可兼容使用。每项订阅有独立 ID，通知使用订阅时请求的名称。`track-selection-changed` 和 `trackSelectionChanged` 返回相同的 Native Monitor 数据，包括 `invalidated` 和 `available`。

订阅 `transport-state-changed` 时，能获取工程主速度时会返回 `tempo`（BPM）。旧名称 `transportChanged` 保留原有字段和通知行为。

Service Event 额外包含 `service`。Service 卸载时拒绝待完成 invoke，发送 `unloaded` 并移除相关订阅。收到 `projects-state-changed` 时，若有活动工程，`activeProject` 对应 `projects[]` 中的一项 ID。ID 仅在当前连接内可比对，重新加载工程或重连后须使用新快照中的 ID。每个 Session 最多 256 项订阅。

## 二进制 Stream 与生命周期

`stream.open` 为已注册 Producer 创建 Consumer。`endpoint` 是包含一次性 `ticket` 的完整 WebSocket URL，须在十秒内连接。Ticket 绑定 External Session 与目标 Stream，不依赖 WebView Origin。Access Token 与 Stream Ticket 是不同凭据。

数据使用现有 [Native Stream 二进制协议](native-streams.zh-CN.md)，消费 packet 后发送二进制字节 `0x01` 确认。控制连接不传输 Stream packet。原有 WebView Stream 保持 Origin 校验。共享 Stream Hub 最多允许所有调用方合计 64 个 Consumer。

断开、关闭服务、重新生成 Token 或卸载扩展时，清理对应 Session 的待完成 invoke、订阅、工程 ID 与 Consumer，不注销共享 Service，也不停止其他调用方使用的 Producer。调用方共享 REAPER 状态和原生资源限制。

控制请求和响应上限为 64 MiB。每连接最多 128 个待完成请求、256 条待发送消息，收发队列各有 64 MiB 预算。响应使用保守内存估算，深度嵌套的数据可能在序列化达到该大小前被拒绝。发送停滞超过 30 秒断开连接。主要原生限制和当前注册内容可通过 `system.getCapabilities` 查询。

## Demo

在浏览器打开[独立 Demo](../examples/integrations/external-client/README.md)，可验证认证、能力查询、通过 runtime 服务读取工程和播放状态、订阅事件和读取已有二进制流。
