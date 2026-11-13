# 工程视频预览

`reaper.video` 通过 Native Frame Streams 或 Windows、macOS 原生呈现表面提供当前工程的最终视频。预览包含工程的视频处理效果。

视频演示位于 `examples/integrations/video-preview`，运行 [reawebapi-video-preview.lua](../examples/integrations/video-preview/reawebapi-video-preview.lua) 启动。视频演示不包含在 ReaPack 中。

## 主机兼容性

安装同平台、同架构的 v0.3.9.40 或更新版本扩展。视频后端校验规范化模块指纹，不检查 REAPER 版本号。指纹排除指定构建元数据字段，保留代码、数据和布局校验，用于保护固定内部地址的调用。

后端支持 Windows x64、macOS arm64 和 Linux x86_64。规范化排除 PE 构建时间戳、校验和与 CodeView 标识，Mach-O UUID 与代码签名载荷，以及 ELF GNU build ID。Linux 按匹配的模块布局选择地址映射，覆盖 REAPER 7.82、7.84 和 7.85。

`getCapabilities()` 查询后端是否具备启动条件及共享配置限制，后端名称为 `reaper-internal`。视频模块指纹不匹配、模块无法读取、缺少必要宿主接口或当前平台/架构没有后端实现时，`available` 为 `false`，`openStream()` 返回 `VIDEO_HOST_UNSUPPORTED`，示例显示不可用提示。无需桌面录制权限。

只有规范化模块指纹与固定内部地址匹配时才允许调用。仅上述规范化字段变化的模块无需新增版本记录即可通过。代码、数据或布局变化仍需验证兼容性。此检查不替代实机验收。

## API

```js
await reaper.lifecycle.ready;
const videoArea = document.querySelector('#stage');
const canvas = videoArea.querySelector('canvas');
const context = canvas.getContext('2d');
const caps = await reaper.video.getCapabilities();
if (!caps.available) throw new Error(caps.error);
const stream = await reaper.video.openStream({
  source: 'project', resolution: 'fit', preserveAspect: true,
  width: Math.round(videoArea.clientWidth * devicePixelRatio),
  height: Math.round(videoArea.clientHeight * devicePixelRatio), updateRate: 'auto'
});
canvas.width = stream.info.width;
canvas.height = stream.info.height;
stream.on('data', frame => {
  if (canvas.width !== frame.width || canvas.height !== frame.height) {
    canvas.width = frame.width;
    canvas.height = frame.height;
  }
  context.putImageData(new ImageData(new Uint8ClampedArray(
    frame.bytes.buffer, frame.bytes.byteOffset, frame.bytes.byteLength
  ), frame.width, frame.height), 0, 0);
});
const clear = () => context.clearRect(0, 0, canvas.width, canvas.height);
stream.on('error', error => { clear(); console.error(error.code, error.message); });
stream.on('close', clear);
await reaper.lifecycle.on('cleanup', () => stream.close());
```

`getSourceInfo()` 返回最终视频帧的 `width`、`height`、`available`、`originalSupported` 和 `error`，以及可为空的 `nativeFormat`、`colorMatrix`、`colorRange`、`colorConversion`，不根据素材文件名或工程帧率推断尺寸。等待解码或没有视频时，尺寸为零。未打开流时，尺寸探测最多保留后端五秒，以等待异步解码完成。

| `resolution` | 输出规则 |
| --- | --- |
| `fit` | 必须提供视频区域的物理像素宽高，即 CSS 尺寸乘设备像素比。在此范围内适配源尺寸，不放大。 |
| `source` | 使用最终帧的真实宽高，不接受 `width`、`height`。超出预算时明确报错。 |
| `custom` | 使用指定宽高。保留旧调用方式，省略尺寸时仍使用兼容默认值 1280×720。 |

`preserveAspect` 默认为 `true`，自定义输出通过不透明黑边保持比例。设为 `false` 时允许拉伸。同尺寸转换保留 RGB 像素值，缩放使用双线性滤波。原始分辨率不代表新增 HDR、高位深或显示色彩管理支持，输出仍为 RGBA8。

`fit`、`source` 必须等到有效解码帧。收到 `VIDEO_FRAME_PENDING` 时应稍后重试，不应代入虚构源尺寸。源尺寸变化时，这两种模式在当前流内更新输出尺寸，无需重新连接。`fit` 按最初请求的视口边界重新适配且不放大，主动修改视口参数或分辨率模式仍需重新打开。自定义流保留指定尺寸。共享配置比较模式、请求的视口或自定义尺寸、宽高比策略及更新率，冲突请求不会替换其他消费者的流。

`fit`、`source` 的描述包含 `dynamicResolution: true` 和 `layoutVersion: 3`。`info.width`、`info.height`、`info.stride` 是连接时的快照，读取每帧时使用 `frame.width`、`frame.height`、`frame.stride`，有效数据长度为 `frame.bytes.byteLength`。`info.maxBytes` 表示 40 MiB 容量上限，不代表当前帧长度。自定义流保留固定尺寸和布局版本 1。

返回对象直接复用 Native Stream，支持 `latest()`、`on('data' | 'error' | 'close')` 和 `close()`。有效视频帧的时间戳是 REAPER 工程时间，单位为秒。WebGL 可将 `frame.bytes` 上传为 RGBA8 纹理。使用 `requestAnimationFrame` 合并绘制更新。

Demo 的网页绘制路径使用持续的 `requestAnimationFrame` 循环，按顺序缓存最多六个包且不超过 128 MiB，每次刷新绘制一个，溢出时丢弃最旧包。启动等待第二个包或约 16 ms，相当于 60 FPS 下约一帧，停止状态下的单个帧也会显示。工程视频失效包携带 `discontinuity: true`，用于清除变化前的排队帧，即使工程位置相同也会生效。时间戳倒退或向前跳变超过上次不同宿主位置以来的实际时间 150 ms 时重置队列。宿主位置重复后正常追赶时钟不视为跳转。关闭或错误会取消待绘制内容。缓存可平滑到达节奏，但会增加延迟，实际呈现仍受浏览器和屏幕刷新率限制。

工程视频按顺序保留最多三个已采集帧。原始套接字消费者允许三个帧等待确认，共享内存消费者使用两个。两条路径均允许两个帧等待发送。待发送队列满时丢弃最旧帧，避免慢消费者无限积累延迟或阻塞其他消费者。工程视频交换已有采集缓冲，发送池按实际分配容量限制在 240 MiB 内。套接字在途帧须为当前配置的下一帧发布预留空间，避免慢套接字耗尽原生内存消费者所需的发送存储。切换至更大帧时，可能需要等待旧在途帧释放容量。压力下先回收可替换的待发送快照。仍被消费者引用的缓冲不会被覆写。其他 Native Stream 的缓冲策略保持不变。

支持共享内存的 Windows WebView2 为每个项目视频消费者分配两个只读槽位。macOS 通过窗口独占的本地资源地址传输相同的原始 RGBA 像素，省去一次宿主侧复制。Linux 通过匹配的 WebKit 视频模块映射只读原生槽位，将每帧复制一次到浏览器拥有的内存。安装时须保留 `UserPlugins/reawebapi-video-<arch>/reawebapi-video-<arch>.so`。套接字元数据与 ACK 共同保护两个槽位，SDK 完成像素复制后才确认，保留包不受槽位复用影响。动态流每个消费者预留两个 40 MiB 原生槽位，地址保持不变，仅复制每帧有效像素。原生槽位总预算另限为 256 MiB，不支持原生路径或预算不足时保留原始二进制套接字传输。其他流的传输方式不变。

`getState()` 返回源尺寸、最新采集帧的工程位置、帧龄、订阅数和可用状态。`getDiagnostics()` 另包含采集与生产帧数、清屏次数 `clearedFrames`、采集失败、丢帧、平均采集 FPS 和最近一次成功采集帧的耗时 `captureMs`，空轮询不覆盖该耗时。`actualFPS` 统计成功采集次数，不代表不同画面或实际显示帧数。静态画面与低帧率素材仍可能产生视觉内容相同的采集帧。清屏不计入 `capturedFrames`，成功发布的清屏计入 `producedFrames`。`transportLatencyMs` 是任一消费者最近一次帧发送至 ACK 返回的耗时，包含页面回调处理，不包含 Canvas 或 GPU 呈现。首个 ACK 到达前为 `null`。其他传输诊断由 `reaper.stream.getDiagnostics()` 提供。

## 帧失效与状态

新流先按初始输出尺寸发布不透明黑色清屏。动态预览在等待解码期间保留上一有效画面最多 500 ms，快速定位时从最新请求重新计时，新帧到达后直接替换。自定义预览保留工程编辑、定位及播放状态变化时的即时失效行为，普通解码等待在播放时最多保留 500 ms，停止时为 100 ms。缺少有效源或 REAPER 渲染占用时仍会清屏。清屏包用于清除保留画面，不代表已取得有效视频源。

`status === 'ready'` 表示有有效源帧，包括短暂等待解码时保留的帧。清屏后 `sourceWidth` 和 `sourceHeight` 为 `0`，`position` 为 `null`。不要只依据包到达或像素是否为黑色判断有效性。流关闭或发生错误时，页面应主动清空 Canvas。

| 错误码 | 行为 |
| --- | --- |
| `VIDEO_RESOLUTION_LIMIT` | 打开时拒绝超限尺寸，动态流运行中清屏并保留连接，尺寸恢复后继续输出 |
| `VIDEO_FRAME_PENDING` | 等待与当前工程状态一致的帧 |
| `VIDEO_SOURCE_UNAVAILABLE` | 当前没有可用源帧或帧格式无效 |
| `VIDEO_RENDER_BUSY` | REAPER 渲染占用，暂停预览取帧并等待恢复 |
| `VIDEO_PROJECT_CHANGED` | 旧流关闭，需为当前工程重新打开 |
| `VIDEO_HOST_UNSUPPORTED` | 后端启动条件不满足，拒绝打开流并提示不可用 |

## 原生 I420

`format` 默认为 `rgba8`。转换器已验证的 Windows x64、macOS ARM64 和 Linux x86_64 主机可使用 `format: 'i420'` 与 `resolution: 'source'`，直接复制最终原生 Y/U/V 平面，不进行 RGB 转换或新增色度采样。宽度须为 16 的倍数，高度须为偶数，目前已验证的转换为有限范围 BT.709。Windows 对应已验证的 FFmpeg 54、62 SSE 转换，Linux x86_64 对应已验证的 FFmpeg 60 SSE 转换，macOS ARM64 对应原生 NEON 转换，其他转换器配置保留 RGBA。其他原生格式、矩阵和平台返回 `VIDEO_FORMAT_UNSUPPORTED`。`capabilities.formats` 列出可用路径。`sourceInfo.colorMatrix` 也可识别 BT.601/BT.2020，但不代表开放对应 I420 输出。

I420 数据包依次包含完整尺寸 Y 平面及各占四分之一尺寸的 U、V 平面，不含行填充。`frame.stride === frame.width`，`frame.bytes.byteLength === frame.width * frame.height * 3 / 2`。`info.maxBytes` 仍为 40 MiB 容量上限。描述携带 `colorMatrix: 'bt709'`、`colorRange: 'limited'` 和 `colorConversion: 'reaper-sse' | 'reaper-neon'`。复现 CPU RGBA 读回值时，使用对应转换器的整数舍入。像素预算仍按等尺寸 RGBA 计算。格式纳入首个消费者的共享配置。符合约束的尺寸变化保留 I420 流。原生格式、色彩矩阵或转换器不再满足当前配置时关闭流，视频状态报告 `VIDEO_FORMAT_CHANGED`，消费者应重新打开或回退 RGBA。

Demo 的网页绘制路径保留原始分辨率 I420 纹理，并按实际显示像素绘制。Windows D3D11 和 Demo 的 Windows WebGL 回退路径使用 REAPER OpenGL 的 BT.709 显示矩阵，按显示尺寸采样 YUV 平面并在缩小时使用面积加权滤波，再转换并量化 RGB。原始像素模式保持 1:1 绘制，显示尺寸变化不改变视频流尺寸。未支持的源与 WebView 保留 RGBA 绘制。

## Windows 与 macOS 原生呈现

Windows x64 与 macOS ARM64 分别使用 D3D11 和 Metal，支持 RGBA8 纹理直接上传和上述已验证的 I420 转换。RGBA8 无需再次转换 RGB。

`getCapabilities().nativePresentation` 返回 `{available, backend: 'd3d11' | 'metal' | null}`。可用时，`openSurface(options?)` 在页面 HTML 内容下方打开原生视频层。它接受视频配置参数，相同配置的原生预览共享采集与帧上传。Windows I420 表面按各自显示区域采样共享的 YUV 平面。REAPER 对高清 I420 使用 DirectDraw 输出时，原始分辨率的 D3D11 表面可独立于工程矩阵，选择 BT.709 显示路径（`colorConversion: 'reaper-directdraw'`）。该配置仅用于原生呈现，硬件舍入可能产生 1 级 RGB 差异。Metal 表面共享转换后的 RGB 纹理。视频区域上方的页面背景需透明，覆盖的控件应设置独立背景，可参考 Demo。现有 `openStream()` 像素访问、Windows 工作线程接收及 Linux 网页绘制路径保留。

返回表面提供冻结的 `info`，包含连接时的视频描述和 `renderer: 'native-d3d11' | 'native-metal'`，只读的 `closed`、`error` 与 `readablePixels: false`，以及异步 `setLayout({rect, clip, preserveAspect})`、`getDiagnostics()` 和 `close()`。`rect` 与 `clip` 均包含有限数值 `x`、`y`、`width`、`height`，使用 CSS 视口坐标，不乘设备像素比。宽高必须非负，零宽高隐藏画面。`preserveAspect` 默认为 `true`。`on('close' | 'error', callback)` 返回取消订阅函数。不提供像素事件、`latest()` 或 `read()`，`info` 不暴露传输令牌或 URL。原生表面不是 Canvas，Canvas 像素读取及网页快照不包含其原生画面。

不可用的平台返回 `VIDEO_PRESENTATION_UNAVAILABLE`。每个窗口最多拥有一个表面，重复打开返回 `VIDEO_SURFACE_IN_USE`，更换配置前应关闭当前表面。布局变化仅调整显示尺寸与裁剪，不改变采集尺寸。Demo 的原生适配模式请求 `resolution: 'source'` 并适配显示布局，保留完整源纹理，使不同大小的窗口可共享同一配置。自定义模式仍按指定尺寸采集。源尺寸变化保留流和原生设备，按可复用槽位调整纹理。视口不变时保留 Windows 交换链或 Metal layer。当前原生输入尺寸从 `getDiagnostics()` 的 `sourceWidth`、`sourceHeight` 读取。

原生呈现的全屏容器需设置透明的 `::backdrop`，全屏期间隐藏周围页面内容。控件间距应使用不透明背景内部的 padding，避免 margin 暴露底层背景。Demo 仅在原生呈现模式应用这些样式。

原生呈现诊断统计提交与呈现活动，不代表屏幕实际扫描输出。`sourceUploadedFrames`、`sourceConvertedFrames` 与 `sourceDroppedFrames` 属于共享源，不应跨表面累加。Windows 的 `sourceConvertedFrames` 在每个 I420 帧首次绘制时计数。已分离的表面返回 `{closed: true}`，呈现失败时可附带 `errorCode` 与 `error`。吞吐量仍受源解码、GPU 负载与屏幕刷新率限制，不保证任意数量窗口均达到固定帧率。

Windows 返回 `presentationCounter: 'dxgi-observed'`，仅统计 DXGI 呈现信息中观察到的新源帧，不以 `Present()` 成功次数或提交编号差值推断显示帧数，也不补计未观察到的中间帧，因此计数与 FPS 可能低于屏幕实际呈现值。统计不可用时，`presentationAvailable` 为 `false`，`presentedFrames` 与 `presentationFPS` 为 `null`，`hasFrame` 不依赖这些计数。macOS 统计 Metal 呈现回调。这些指标不能证明每一帧都已在屏幕上可见。

原生呈现不可用或报告 `VIDEO_RENDERER_UNAVAILABLE` 时，Demo 回退网页绘制。Windows 使用现有工作线程路径，macOS 保留网页绘制回退。SDK 不会将 `openSurface()` 结果隐式变成像素流，应用应显式选择回退方式。原生呈现不启用 WebView2 实验纹理流，也不改变普通窗口的绘制设置。

Canvas 预览在适配显示区域时如因缩放重新连接，Demo 会保留上一帧，直到新连接绘制首帧。

## Windows 视频工作线程

`openWorkerStream(worker, options?)` 将 WebView2 视频映射直接交给专用 `Worker`，在工作线程接收和上传像素，省去页面主线程的整帧复制。返回连接提供 `info`、`closed`、`error`、`on('data'|'error'|'close', callback)` 和 `close()`。数据事件仅含帧元数据，不提供 `bytes`、`data`、`latest()` 或 `read()`。原有 `openStream()` 仍返回独立持有、内容稳定的数据包，默认格式为 RGBA。

工作线程协议参考示例中的 `video-worker.js` 和 `worker-stream.js`。SDK 发送 `{type:'reaweb-video', port, descriptor, buffer}`，独立 `MessagePort` 传递 `ready`、元数据 `frame`、`error`、`closed` 响应及 `close` 请求。共享槽位在同步 GPU 上传完成或丢弃后才 ACK。关闭时清理队列，通过 `closed` 归还映射后再解除订阅。没有原生映射时使用现有二进制视频通道，不按页面数量限制连接。

终止或复用工作线程前应等待 `connection.close()`。工作线程失败或关闭超过一秒时，SDK 终止该专用线程并解除订阅。页面隐藏和文档清理也会关闭连接。该方法需要 WebView2，其他引擎返回 `UNSUPPORTED_FORMAT`。Demo 将其用于 Windows 网页绘制路径。Windows 与 macOS 在可用时优先使用原生呈现，Linux 保留网页绘制。

## 资源与限制

- `updateRate: 'auto'` 跟随当前工程帧率，保留 23.976、29.97 等小数精度，工程帧率改变时无需重新打开流。目标限制为 1–75 FPS，无法读取有效工程帧率时回退到 30 FPS。数字参数接受固定的 1–75 FPS，支持小数帧率，省略参数与 Demo 均默认使用 `auto`。
- 状态和诊断中的 `updateRate` 表示请求模式，`targetUpdateRate` 表示当前目标更新率。`stream.info.updateRate` 是创建流时的数值快照。实际输出受源解码、宿主负载和绘制性能影响，显示帧率还受屏幕刷新率限制。自动与固定模式属于不同共享配置，即使当前目标帧率相同也不能混用。
- 当前工程只保留一个共享配置。首个消费者确定分辨率和更新率。后续请求不一致时返回 `VIDEO_PROFILE_CONFLICT`。旧自定义调用的兼容默认值为 1280×720、自动跟随工程帧率，单边上限 8192，RGBA 单帧预算 40 MiB（10,485,760 像素），帧率上限 75 FPS。
- 最终源尺寸变化时，适配与原始模式在流内调整输出尺寸。自定义模式保持指定尺寸，并按宽高比策略决定黑边或拉伸。独立视频调度器唤醒 REAPER 主线程，不依赖扩展定时器频率。工程帧率不超过 75 FPS，且以正常或更慢速度播放时，auto 直接跟随宿主的新帧。固定更新率、加速播放和更高工程帧率仍按目标速率限流。内部视频调用、像素缩放复制和发布仍在该线程执行。帧传输复用现有有界二进制流。
- Windows 在工程视频流活动期间使用高精度等待，减少传输线程的轮询与 ACK 延迟，不可用时回退到普通等待。系统定时器分辨率与 ACK 协议保持不变。
- 连续播放时优先读取尚未采集的官方已选帧，最多允许落后当前查询位置一帧，并仍须通过源与缓存校验。播放期间，根据查询帧索引、源与监视处理哈希及帧时间戳识别未变化的选中帧，避免重复转换和复制。帧未变化或尚未就绪时，在下一次调度唤醒时重试，不再额外等待一个目标更新间隔。停止播放时继续刷新预览，失效后强制重新采集。这不保证每个源帧都能送达。
- REAPER 将原生帧转换为其 RGBA 格式，内存字节顺序为 BGRA。扩展再复制、缩放并转换为网页 RGBA8，保留宿主的 YUV 转换，不独立选择颜色矩阵或重新解码素材。源帧引用仅在采集期间持有，复制完成后释放，流只接收独立的像素数据。
- 视频处理按需启用，最后一个消费者断开后释放后端调度状态。未连接的流在十秒后过期。关闭、重载和卸载复用现有清理机制。
- App 或页面隐藏时自动关闭其视频消费者，包括会隐藏页面的 Dock 切换。恢复可见后可重新打开。隐藏页面打开流返回 `VIDEO_PAGE_HIDDEN`。
- 切换工程会关闭旧流并递增源 epoch。App 应在工程切换后重新打开，显示保留帧前应查询当前状态。

后端依赖 REAPER 非公开函数和状态，跨版本兼容性仍需实机验证。RGBA 和 YV12 输入路径已验证，YUY2 交由同一宿主转换路径处理，尚未单独进行素材验收。原生窗口的缩放、显示色彩管理和 GPU 呈现可能与采集到的源像素不同。

`tests/video_reaper.py` 使用全新隔离配置执行真实主机验收。编译和单元测试不能替代主机视频兼容性验证。

`tests/video_cadence.py` 检查采集、接收和绘制回调，不验证屏幕最终呈现。对恒定帧率编号素材，应另行采样屏幕并使用 `tests/video_display.py` 分析 CSV 或含 `samples` 的 JSON，明确指定编号列、循环周期和最低帧率。分析同时检查编号缺失与倒退。使用前需校准采样工具并确认窗口可见、屏幕唤醒，软件虚拟显示结果不能替代物理屏幕验收。

## Demo 显示与全屏

Demo 打开后自动预览，默认适配视频区域。选项菜单统一提供显示、输出分辨率和外观设置，默认深色，可切换并记住浅色或深色。原生标题栏图标隐藏。视频左右贴合页面边缘，控制区保留独立间距。原生适配预览保留源分辨率纹理，窗口和 DPI 变化只更新显示布局，自定义模式仍按指定尺寸采集。网页适配预览对窗口和 DPI 变化采用延迟合并，仅在实际输出尺寸改变时重新打开流。源尺寸变化在当前流内处理，无需重新连接。显示设置提供全屏、保持视频宽高比和原始像素大小开关。双击视频切换全屏，Escape 或画面上的退出按钮返回。

原始像素大小模式选择源分辨率输出，一个视频像素对应一个屏幕物理像素。浮动窗口在当前屏幕工作区内调整尺寸，关闭此模式时恢复原尺寸。Docker 中不调整宿主容器，超出区域的画面允许滚动。

Windows、Linux 使用原生全屏容器，macOS 使用 WebKit 元素全屏。Linux 需要支持全屏的 X11 窗口管理器。无法全屏时显示明确错误，不以最大化代替。页面分别显示源尺寸、流尺寸、Canvas 显示框尺寸，以及目标、采集、接收和绘制帧率。Canvas 提交次数不等于屏幕实际扫描输出次数。

单帧预算覆盖 UHD 与 DCI 4K，但不按分辨率名称分支。动态流的三个采集槽位分别按 40 MiB 计入共享的 256 MiB 生产者预算。传输输出池按实际分配容量限制为 240 MiB，不能回收时丢弃新包，不无限分配。`VIDEO_RESOLUTION_LIMIT` 表示尺寸或像素预算超限，不会静默降为 720p。`VIDEO_PROFILE_CONFLICT` 表示共享配置冲突，不影响其他消费者。

高分辨率 RGBA 传输增加内存和复制带宽，支持原始 4K 不等于保证 60 FPS。应使用实际素材、输出模式、显示设备和音频设备组合验收。

Demo 提供 EN / CN 语言切换。网页绘制回退路径中，已验证的原始 I420 帧使用单纹理 GPU 上传，保留对应宿主转换器的整数舍入。Windows I420 优先使用 OffscreenCanvas 工作线程，macOS 和 Linux I420 使用主线程 WebGL2。macOS 的 RGBA 路径在可用时保留无压缩 ImageBitmap 呈现。其他 RGBA 路径在支持时使用 OffscreenCanvas 工作线程，并保留主线程 WebGL 或 Canvas 2D 回退。Demo 读取包元数据后将像素所有权交给工作线程，等待工作线程处理的提交最多六帧。诊断区区分传输序号缺口与绘制队列丢帧，帧率仍受硬件及显示刷新率限制。Demo 在 `app.json` 中启用 `videoHighRefreshRate: true`。macOS 在创建视频窗口时，通过可用性检查后的私有特性接口关闭 WebKit 接近 60 Hz 的页面调度偏好。该选项默认为 `false`，其他平台保持原有调度。修改后须重新打开窗口。不支持该接口的 WebKit 保留系统调度，在 144 Hz 屏幕上仍可能限制为约 72 FPS。该设置不改变源帧率，也不保证显示吞吐量。见 [WebKit 调度说明](https://bugs.webkit.org/show_bug.cgi?id=221673)。

非可编辑输入区域内，未组合修饰键的空格通过既有 DAW 键盘转发控制 REAPER 播放，由 Demo 清单启用。Windows 退出全屏后恢复浮动窗口与 REAPER 的归属关系。
