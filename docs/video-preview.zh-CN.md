# 工程视频预览

`reaper.video` 通过 Native Frame Streams 传输当前工程的最终视频。预览包含工程的视频处理效果。

Canvas 演示位于 `examples/integrations/video-preview`，运行 [reawebapi-video-preview.lua](../examples/integrations/video-preview/reawebapi-video-preview.lua) 启动。视频演示不包含在 ReaPack 中。

## 主机兼容性

安装同平台、同架构的 v0.3.9.28 或更新版本扩展。视频后端不检查 REAPER 版本号，也不返回支持版本列表；保留视频模块 SHA-256 校验，用于保护固定内部地址的调用。

`getCapabilities()` 查询后端是否具备启动条件及共享配置限制，后端名称为 `reaper-internal`。视频模块指纹不匹配、模块无法读取、缺少必要宿主接口或当前平台/架构没有后端实现时，`available` 为 `false`，`openStream()` 返回 `VIDEO_HOST_UNSUPPORTED`，示例显示不可用提示。无需桌面录制权限。

只有模块文件与固定内部地址对应的 SHA-256 匹配时才允许调用。不同 REAPER 版本使用相同视频模块时可以通过；相同版本号的不同模块文件也可能被拒绝。此检查不替代实机兼容性验证。

## API

```js
await reaper.lifecycle.ready;
const caps = await reaper.video.getCapabilities();
if (!caps.available) throw new Error(caps.error);
const stream = await reaper.video.openStream({
  source: 'project', width: 1280, height: 720, updateRate: 'auto'
});
canvas.width = stream.info.width;
canvas.height = stream.info.height;
stream.on('data', frame => {
  context.putImageData(new ImageData(new Uint8ClampedArray(
    frame.bytes.buffer, frame.bytes.byteOffset, frame.bytes.byteLength
  ), stream.info.width, stream.info.height), 0, 0);
});
const clear = () => context.clearRect(0, 0, canvas.width, canvas.height);
stream.on('error', error => { clear(); console.error(error.code, error.message); });
stream.on('close', clear);
await reaper.lifecycle.on('cleanup', () => stream.close());
```

返回对象直接复用 Native Stream，支持 `latest()`、`on('data' | 'error' | 'close')` 和 `close()`。有效视频帧的时间戳是 REAPER 工程时间，单位为秒。WebGL 可将 `frame.bytes` 上传为 RGBA8 纹理。使用 `requestAnimationFrame` 合并绘制更新。

`getState()` 返回源尺寸、最新采集帧的工程位置、帧龄、订阅数和可用状态。`getDiagnostics()` 另包含采集与生产帧数、清屏次数 `clearedFrames`、采集失败、丢帧、平均采集 FPS 和采集耗时。清屏不计入 `capturedFrames`，成功发布的清屏计入 `producedFrames`。`transportLatencyMs` 是任一消费者最近一次帧发送至 ACK 返回的耗时，包含页面回调处理，不包含 Canvas 或 GPU 呈现。首个 ACK 到达前为 `null`。其他传输诊断由 `reaper.stream.getDiagnostics()` 提供。

## 帧失效与状态

新流先发布固定尺寸的不透明黑色清屏。工程编辑、定位或播放状态变化会使旧帧失效，缺少有效源、等待解码或 REAPER 渲染占用时，也会在有效画面失效时发布清屏。清屏包用于清除消费者保留的画面，不代表已取得有效视频源。

`status === 'ready'` 表示有有效源帧。等待期间 `sourceWidth` 和 `sourceHeight` 为 `0`，`position` 为 `null`。不要只依据包到达或像素是否为黑色判断有效性。流关闭或发生错误时，页面应主动清空 Canvas。

| 错误码 | 行为 |
| --- | --- |
| `VIDEO_FRAME_PENDING` | 等待与当前工程状态一致的帧 |
| `VIDEO_SOURCE_UNAVAILABLE` | 当前没有可用源帧或帧格式无效 |
| `VIDEO_RENDER_BUSY` | REAPER 渲染占用，暂停预览取帧并等待恢复 |
| `VIDEO_PROJECT_CHANGED` | 旧流关闭，需为当前工程重新打开 |
| `VIDEO_HOST_UNSUPPORTED` | 后端启动条件不满足，拒绝打开流并提示不可用 |

## 资源与限制

- `updateRate: 'auto'` 跟随当前工程帧率，保留 23.976、29.97 等小数精度，工程帧率改变时无需重新打开流。目标限制为 1–60 FPS，无法读取有效工程帧率时回退到 30 FPS。数字参数仍为固定的 1–60 整数 FPS，省略参数与 Demo 均默认使用 `auto`。
- 状态和诊断中的 `updateRate` 表示请求模式，`targetUpdateRate` 表示当前目标更新率。`stream.info.updateRate` 是创建流时的数值快照。实际输出受宿主负载和绘制性能影响。自动与固定模式属于不同共享配置，即使当前目标帧率相同也不能混用。
- 当前工程只保留一个共享配置。首个消费者确定分辨率和更新率。后续请求不一致时返回 `VIDEO_PROFILE_CONFLICT`。默认 1280×720、自动跟随工程帧率，上限 1920×1080、60 FPS。
- 输出尺寸固定，保持源宽高比并填充不透明黑边。源尺寸变化不改变流描述。内部视频调用、像素缩放复制和发布均在 REAPER 主线程执行。帧传输复用现有有界二进制流。
- REAPER 将原生帧转换为其 RGBA 格式，内存字节顺序为 BGRA。扩展再复制、缩放并转换为网页 RGBA8，保留宿主的 YUV 转换，不独立选择颜色矩阵或重新解码素材。源帧引用仅在采集期间持有，复制完成后释放，流只接收独立的像素数据。
- 视频处理按需启用，最后一个消费者断开后释放后端调度状态。未连接的流在十秒后过期。关闭、重载和卸载复用现有清理机制。
- App 或页面隐藏时自动关闭其视频消费者，包括会隐藏页面的 Dock 切换。恢复可见后可重新打开。隐藏页面打开流返回 `VIDEO_PAGE_HIDDEN`。
- 切换工程会关闭旧流并递增源 epoch。App 应在工程切换后重新打开，显示保留帧前应查询当前状态。

后端依赖 REAPER 非公开函数和状态，跨版本兼容性仍需实机验证。RGBA 和 YV12 输入路径已验证，YUY2 交由同一宿主转换路径处理，尚未单独进行素材验收。原生窗口的缩放、显示色彩管理和 GPU 呈现可能与采集到的源像素不同。

`tests/video_reaper.py` 使用全新隔离配置执行真实主机验收。编译和单元测试不能替代主机视频兼容性验证。
