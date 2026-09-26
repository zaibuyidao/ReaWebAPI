# Native Stream

`reaper.host` 对应 Lua Backend，`reaper.host.service(name)` 对应原生服务，`reaper.events` 对应状态通知，`reaper.stream` 对应连续二进制数据。现有接口保持原有契约。

```js
const stream = await reaper.audio.openStream('spectrum', {
  source: 'master', fftSize: 2048, updateRate: 30
});
stream.on('data', packet => render(packet.data, stream.info));
stream.on('close', error => console.log(error.code));
await stream.close();
```

`stream.open(name)` 建立持续二进制连接。`latest()` 读取已到达页面的缓存，不发起 RPC。`read()` 按 FIFO 消费 Audio/MIDI，空队列返回 `null`。Frame、Spectrum、Meter、Waveform 和 Binary 保留最新数据。`stream.close()` 只分离当前 consumer，原生 `ReaWeb_CloseStream()` 关闭整个 producer。

传输由独立原生线程负责，不经过 Lua、逐帧 JSON/Base64、普通 RPC 或 `Runtime::tick()` 帧调度。每个 Stream 使用有界缓存。Publish 不分配内存、不加锁、不等待页面。Audio/MIDI 缓存满时丢弃新包，其他类型丢弃过时包。原生丢包计数、序号间断和 consumer `dropped` 可用于诊断。

公共 C ABI 见 [reaweb_stream.h](../src/public/reaweb_stream.h)，示例见 [synthetic producer](../tests/native_stream_extension.cpp)。创建与关闭在 REAPER 主线程执行，每个 Stream 由单个 producer 发布。第三方扩展应注册 `ReaWeb_SetServiceShutdown` 回调，先停止并等待 producer 退出，再关闭 Stream、注销 Service、释放资源。低延迟输入方法可通过 `ReaWeb_SetServiceInput` 注册，回调只更新最新输入状态，不执行耗时命令。

内建 `audio.openStream` 提供 PCM、FFT、Peak/RMS/LUFS 和实时 min/max 波形。`master` 采集硬件输出 0/1，`input` 采集硬件输入 0/1。`selected-track` 或 `track:<GUID>` 使用绑定轨道的 **pre-FX Audio Accessor**，不代表轨道 FX 后或实时输入电平。Accessor 在主线程采样，分析和传输在原生线程执行。已有 `audio.getTrackMeter` 保留 REAPER 瞬时 Peak 语义。文件概览、缓存峰值与缩放查询继续使用 `audio.getWaveform`。

MIDI 通过 `system.openMIDIInput(device)` 消费原始事件，`-1` 表示所有输入。`system.getDevices` 与 `devicesChanged` 提供设备信息。显示器几何、工作区和有效 DPI 通过 `system.getDisplays` 获取，CPU 和 Stream 统计通过 `debug.getDiagnostics` 获取。

`fs.watch` 在原生 worker 每 250 ms 比较文件状态，报告创建、修改、重命名、删除和溢出。短时变化可能合并，页面无需扫描目录。`clipboard.readBinary/writeBinary` 使用自定义 MIME 格式，不自动转换系统图片格式。`system.schedule` 提供一次性和重复原生定时器，适合后台刷新与延时任务，不充当帧时钟。页面关闭或导航后自动释放 consumer、监听和定时器。

完整字段、布局、线程规则、容量、溢出策略和平台坐标差异见 [English contract](native-streams.md)。[Native Stream Demo](../web/native-stream/README.md) 可消费内建分析和第三方 Stream。ReaGBA 的 Service、输入与 `reagba.video` 接入代码保留在独立 ReaGBA 仓库，模拟器按自身约 59.73 Hz 节奏发布画面。

页面如设置 Content Security Policy，需要允许 `connect-src ws://127.0.0.1:*`。连接仍受页面来源及一次性原生连接凭据约束。
