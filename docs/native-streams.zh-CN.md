# Native Stream

[External Client](external-clients.zh-CN.md) 通过绑定 Session 的 Ticket 接入同一 Stream Hub。二进制 packet、确认、背压和原有 WebView Origin 校验保持不变。

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

内建 `audio.openStream` 提供 PCM、FFT、Peak/RMS/LUFS 和实时 min/max 波形。`master` 采集硬件输出，`input` 采集硬件输入。Meter 保留实际通道布局，最多 32 通道。其他分析类型保留原有双声道行为。`selected-track` 或 `track:<GUID>` 使用绑定轨道的 **pre-FX Audio Accessor**，不代表轨道 FX 后或实时输入电平。Accessor 在主线程采样，分析和传输在原生线程执行。已有 `audio.getTrackMeter` 保留 REAPER 瞬时 Peak 语义。文件概览、缓存峰值与缩放查询继续使用 `audio.getWaveform`。

轨道 Source 支持 `aggregate: true`，适用于 `audio`、`spectrum`、`meter` 和 `waveform`：

```js
const stream = await reaper.audio.openStream('spectrum', {
  source: 'selected-track', // 也支持 'track:<GUID>'。
  aggregate: true, fftSize: 2048, updateRate: 30
});
```

聚合递归包含根轨、启用 Parent Send 的 Folder 子轨和音频 Receive 上游。静音 Receive 和仅 MIDI 路由不参与。同一源轨只计算一次，循环路由不会重复累加。每个采样块重新检查路由与 Mute/Solo 状态，排除静音轨道、经静音 Folder 路由的分支及 Solo 排除的源内容，支持 Solo Defeat 和 Solo In Place。根轨静音时返回静音。

所有 Accessor 使用相同工程时间、采样率、通道布局和块大小，非 Meter 流保留双声道读取。Native 层先逐 sample 求和 PCM，再执行现有分析，仅创建一个对外 Stream。保留超过 `1.0` 的浮点结果，不归一化、限幅、按轨平均或补偿增益。Item/Take/Lane 播放状态由 REAPER Accessor 返回的 PCM 决定。

Audio、Spectrum 和 Waveform 在轨道、聚合模式、采样率和更新频率相同时复用源采样。Meter 按轨道、聚合模式、采样率和播放复位选项共享独立的连续采样，不受更新频率影响。聚合读取在宿主周期预算耗尽后从下一源轨继续，仅发布同一工程时间位置的完整求和结果。桥接请求采用独立处理预算，避免慢速采样阻塞连接与释放请求。单次 REAPER Accessor 调用无法中断。轨道 Accessor 丢弃偶发的非有限数或 Float32 溢出 PCM 块，恢复时原始 PCM 序号保留间断，分析流重置历史。连续八块无效数据以 `NATIVE_ERROR` 关闭源。其他分析异常仅关闭受影响的流，不会停止其他生产者或后续新建流。

该模式表示 **pre-FX 源 PCM 的同步聚合**，不代表 post-FX、pre-fader、post-fader 或实际轨道输出。不应用轨道或 Send 的增益、声像、相位和通道映射，不创建 FX、Send、Track 或 Undo。`aggregate` 默认为 `false`，省略或显式关闭时保持现有行为。对 `master` 或 `input` 启用聚合会返回 `INVALID_ARGUMENT`。Stream 的 `source` 标识为 `track:<GUID>:pre-fx:aggregate-source`。删除绑定根轨或切换工程会关闭聚合 Stream。

### Built-in Meter / Loudness Analyzer

`audio.openStream('meter', options)` 支持 `forceMono`，默认 `false`，以及 `resetOnPlaybackStart`，默认 `true`。其他流类型不接受这两个选项。`audio.resetMeter(stream.info.name)` 为本窗口打开的内置 Meter 排队执行复位，停止时也会发布清零状态。

内置 Meter 数据为 `Float32Array(13 * C + 14)`，`C = stream.info.channels`，范围为 1–32。前 `7*C+14` 项保留原有偏移，末尾追加 `6*C` 项精确计数分段。全局字段从 `B = 7*C` 开始。双声道共 40 个 Float32。

| 偏移 | 字段 | 功能与单位 |
| --- | --- | --- |
| `0 … C-1` | `samplePeak[C]` | 发布间隔内每通道最大样本绝对值，线性幅度 |
| `C … 2*C-1` | `truePeak[C]` | 发布间隔内每通道 Cockos 插值峰值，线性幅度 |
| `2*C … 3*C-1` | `channelRms[C]` | 发布间隔内每通道 RMS，线性幅度 |
| `3*C … 4*C-1` | `sampleClipCount[C]` | 每通道样本绝对幅度严格大于 1 的累计计数，前缀为 Float32 兼容近似值 |
| `4*C … 5*C-1` | `truePeakClipCount[C]` | 每通道 True Peak 超过 1 的累计计数，每个输入样本最多计一次，前缀为 Float32 兼容近似值 |
| `5*C … 6*C-1` | `channelMaxSamplePeak[C]` | 每通道历史最大 Sample Peak，线性幅度 |
| `6*C … 7*C-1` | `channelMaxTruePeak[C]` | 每通道历史最大 True Peak，线性幅度 |
| `B + 0` | `rmsMomentary` | 400 ms 窗口的通道能量总和，dBFS |
| `B + 1` | `rmsIntegrated` | Cockos RMS-I，每 100 ms 累积重叠的 400 ms RMS 窗口能量，dBFS |
| `B + 2` | `maxRmsMomentary` | 自复位以来最大 RMS-M，dBFS |
| `B + 3` | `lufsMomentary` | K 加权 400 ms 瞬时响度，LUFS |
| `B + 4` | `lufsShortTerm` | K 加权 3 s 短时响度，LUFS |
| `B + 5` | `lufsIntegrated` | 自复位以来 Cockos 门限积分响度，LUFS |
| `B + 6` | `loudnessRange` | `loudnessRangeHigh - loudnessRangeLow`，LU |
| `B + 7` | `maxLufsMomentary` | 自复位以来最大 LUFS-M，LUFS |
| `B + 8` | `maxLufsShortTerm` | 自复位以来最大 LUFS-S，LUFS |
| `B + 9` | `maxSamplePeak` | 所有 `channelMaxSamplePeak` 的最大值，线性幅度 |
| `B + 10` | `maxTruePeak` | 所有 `channelMaxTruePeak` 的最大值，线性幅度 |
| `B + 11` | `processedSeconds` | 自复位以来实际处理的 PCM 帧数除以采样率，秒 |
| `B + 12` | `loudnessRangeLow` | 门限筛选后短时响度的第 10 百分位，LUFS |
| `B + 13` | `loudnessRangeHigh` | 门限筛选后短时响度的第 95 百分位，LUFS |

RMS-M、LUFS-M 使用 400 ms 窗口，LUFS-S 使用 3 s 窗口，窗口与最大值按固定 100 ms PCM 步进更新。RMS-I 累积线性窗口能量，包含启动时的补零窗口，累计四步后提供读数。全局 RMS 对通道能量求和，不平均或 K 加权。静音 RMS/LUFS 及未填满的 M/S 窗口返回负无穷。LRA、峰值、计数和时长初始为零，LRA 上下界初始为 -100 LUFS，再应用 Mono 标定。前缀计数保留 Float32 兼容读数，精确累计值应通过 `audio.decodeMeter` 读取。

True Peak 使用 Cockos 32 抽头窗函数 sinc 插值，低于 96 kHz 使用三个分数相位，其他采样率使用一个相位，滤波延迟为 16 个样本。True Peak 计数对延迟样本和插值相位取最大值，每个输入样本最多计一次。Sample Clip 与 True Peak Clip 独立统计。Sample Peak 及其每通道历史复用 libebur128，两个全局峰值最大值均由通道历史取最大值得到。

LUFS-M/S/I 共用 Cockos K 加权，400 ms/3 s 窗口每 100 ms 更新。LUFS-I 在 0.1 LU 分桶中保留实际能量总和，执行官方绝对及相对门限。LRA 在 3 s 窗口填满后每 100 ms 采集一次 LUFS-S，使用官方绝对门限和 -20 LU 相对门限。门限后至少有 20 个统计值时更新上下界，第 10/95 百分位选择及分桶边界取值均与官方一致。

响度通道按 REAPER 的 L/R/C/LFE/环绕顺序解释。少于六通道时权重均为 1，从六通道开始排除 LFE，所有 LUFS 指标统一使用 sqrt(2) 环绕幅度权重。`forceMono` 将全局 RMS/LUFS、其最大值和 LRA 上下界减去 3 dB，不混合 PCM。

Track Meter 使用轨道通道数。Aggregate Meter 使用根轨及音频连接上游轨道中的最大通道数，布局不随 Mute/Solo 改变，各源按相同通道索引求和。此 pre-FX 聚合仍不应用 Send 通道映射。Master/Input 使用硬件采样通道数，保留单声道。硬件回调未提供通道数时，创建和采集均使用设备启用的输入或输出通道数。超过 32 通道的源提供前 32 通道。通道布局变化以 `UNSUPPORTED_FORMAT` 关闭 Meter，重新打开以获取新元数据。

`integratedMode` 默认 `playback-only`，仅在 REAPER 播放期间累计 RMS-I、LUFS-I 和 LRA。`continuous` 只要收到有效 PCM 就累计这三项。该选项不影响实时指标、峰值历史或削波计数。`resetOnPlaybackStart` 独立控制播放开始复位，需要跨播放开始保留连续历史时设为 `false`。

精确计数区从 `T = 7*C+14` 开始，依次存放 Sample Clip 的低、中、高位通道数组，以及 True Peak Clip 的低、中、高位通道数组。每个计数按 `low + middle*2^24 + high*2^48` 还原，三段分别为 24/24/16 位非负整数，均可由 Float32 精确表示。原生计数使用 uint64，在 `2^64-1` 饱和，不回绕。

```js
const meter = await reaper.audio.openStream('meter', {
  source: 'input', integratedMode: 'continuous', resetOnPlaybackStart: false
});
meter.on('data', ({ data }) => {
  const values = reaper.audio.decodeMeter(data, meter.info.channels);
  console.log(values.lufsIntegrated, values.sampleClipCount[0].toString());
});
```

`audio.decodeMeter(data, C)` 为同步 SDK 解码器，返回具名测量值及 `bigint[]` 类型的 `sampleClipCount` / `truePeakClipCount`，不发起 RPC。写入 JSON 时将 bigint 转为十进制字符串。该解码器仅用于上述内置布局，第三方 Meter 继续自行定义 payload。

轨道和 Aggregate Meter 仅累计播放期间连续向前的 PCM。停止时保留历史，不反复读取 Edit Cursor。Seek、循环回跳、源内容变化和 Accessor 刷新会复位。聚合源中音频时间范围持续为空的 Accessor 反复报告刷新时，不复位历史。添加或移除媒体仍会复位。播放开始默认复位。Master/Input 停止时继续分析实际捕获的 PCM，由 `integratedMode` 控制三项历史累计。Track/Aggregate 停止时不产生新 PCM，选择 `continuous` 也不会反复读取光标处音频。捕获间断和丢包会复位。采样率或通道布局变化以 `UNSUPPORTED_FORMAT` 关闭 Meter，重新打开后使用新元数据和空历史。`processedSeconds` 仅计入实际分析时长。

原 Channel Peak 统一为 `samplePeak`。LUFS-M/S/I 分别使用唯一的 `lufsMomentary`、`lufsShortTerm`、`lufsIntegrated` 计算路径。历史峰值最大值为 `maxSamplePeak` 和 `maxTruePeak`。所有 Meter 复位触发共用 Analyzer 生命周期，同步清空滤波器、窗口、最大值和累计时长。`processedSeconds` 等于 Analyzer 已处理的 PCM 帧数除以采样率，读取和发布不增加时长。

Meter 的 `updateRate` 仅控制发布频率，历史指标不依赖刷新率。`samplePeak`、`truePeak` 和 Channel RMS 保留发布间隔语义。Meter 使用独立的连续源采样，其他音频分析流保留原有采样方式。Native Stream ABI 1、`CreateMeterStream`、`PublishMeter` 和第三方自定义 Meter 布局保持不变。完整偏移表见 [English contract](native-streams.md#built-in-meter--loudness-analyzer)。

MIDI 通过 `system.openMIDIInput(device)` 消费原始事件，`-1` 表示所有输入。`system.getDevices` 与 `devicesChanged` 提供设备信息。显示器几何、工作区和有效 DPI 通过 `system.getDisplays` 获取，CPU 和 Stream 统计通过 `debug.getDiagnostics` 获取。

`fs.watch` 在原生 worker 每 250 ms 比较文件状态，报告创建、修改、重命名、删除和溢出。短时变化可能合并，页面无需扫描目录。`clipboard.readBinary/writeBinary` 使用自定义 MIME 格式，不自动转换系统图片格式。`system.schedule` 提供一次性和重复原生定时器，适合后台刷新与延时任务，不充当帧时钟。页面关闭或导航后自动释放 consumer、监听和定时器。

完整字段、布局、线程规则、容量、溢出策略和平台坐标差异见 [English contract](native-streams.md)。[Native Stream Demo](../examples/integrations/native-stream/README.md) 可消费内建分析和第三方 Stream。ReaGBA 的 Service、输入与 `reagba.video` 接入代码保留在独立 ReaGBA 仓库，模拟器按自身约 59.73 Hz 节奏发布画面。

页面如设置 Content Security Policy，需要允许 `connect-src ws://127.0.0.1:*`。连接仍受页面来源及一次性原生连接凭据约束。

`stream.getDiagnostics().transport` 提供当前 Runtime 的成功连接数、握手拒绝数、过期凭据数及最近的握手拒绝代码，不包含连接凭据。传输尚未初始化时不返回此字段。

## 工程视频

`reaper.video` 通过 Native Streams 提供共享工程视频帧。仅支持已验证的 REAPER 构建。安装、API、绘制和平台限制见[视频预览](video-preview.zh-CN.md)。
