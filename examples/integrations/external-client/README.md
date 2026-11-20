# External Client Demo

Open `index.html` in a browser. Enable **Preferences → Plug-ins → ReaWebAPI → External Clients** in REAPER, apply the settings and copy the token into the Demo.

The browser connects over WebSocket. For clients migrating from the retired mirror, the project buttons invoke `runtime.getProjectSummary` to read the track count and playback state. The other buttons query capabilities, subscribe to events and open binary streams. A producer must register the stream name before **Open stream** can succeed. The token stays in memory.

Disconnect closes this session's subscriptions, pending invokes and stream consumers. Other clients and existing WebView apps remain connected.

直接在浏览器打开 `index.html`。在 REAPER 的 **Preferences → Plug-ins → ReaWebAPI → External Clients** 中启用服务并应用设置，将 Token 复制到 Demo。

工程查询按钮通过具名服务 `runtime.getProjectSummary` 读取轨道数和播放状态。Demo 使用浏览器标准 WebSocket，不需要构建、Lua 启动器、ReaWebAPI WebView 或 JavaScript Mirror。打开 Stream 前，必须有原生 Producer 注册对应名称。断开只释放当前 Session 的资源。
