# 顺序分块文件导出

[English](file-export.md) | **简体中文**

`reaper.fs.openWrite()` 无需构造整文件桥接值即可导出文件。普通 App 和 JSFX WebUI 均可使用，不依赖 App ID、隔离模式或 External Client 端口。现有文件接口及其 16 MiB 单值上限保持不变。

```js
const writer = await reaper.fs.openWrite(destination, {
  overwrite: true,
  expectedBytes: blob.size,
});
try {
  for (let offset = 0; offset < blob.size; offset += writer.maxChunkBytes) {
    const bytes = new Uint8Array(await blob.slice(offset, offset + writer.maxChunkBytes).arrayBuffer());
    await writer.write(bytes);
  }
  const result = await writer.commit(); // {path, bytes}
} catch (error) {
  const state = await writer.abort();
  if (state.state !== 'committed') throw error;
}
```

| 操作 | 契约 |
| --- | --- |
| `fs.openWrite(path, {overwrite?, expectedBytes?})` | 返回当前文档拥有的写入器，默认不覆盖。相对路径基于入口目录，父目录必须存在。拒绝 URL、目录及末级链接。 |
| `writer.write(Uint8Array, {offset?})` | 逐次等待完成。偏移必须等于已确认字节数，返回 `{bytes, totalBytes}`。调用时复制输入字节，不自动重试。 |
| `writer.commit()` | 刷新并发布完整文件，返回 `{path, bytes}`。成功后重复提交返回相同结果。 |
| `writer.abort()` | 取消未发布的写入并尝试清理。已经提交的文件仍保持已提交状态。 |
| `writer.status()` | 返回 `{state, path, bytes, cleanupPending}`。状态为 `open`、`committed`、`aborted` 或 `failed`。 |

`system.getCapabilities().runtime.fs.streamingWrite` 报告实际限制：每块 1 MiB，每文件 64 GiB，每个 Runtime 最多四个活跃写入器，每文档最多一个。文件操作使用独立且有界的执行队列。开始下一次数据操作前需等待写入或提交完成。闲置写入器五分钟后过期，终态记录在最后一次操作后保留五分钟，最多保留 128 个写入器。容量不足返回 `WRITE_BUSY`。

文件先写入目标旁的独占临时目录，发布前不改变原目标。原生写入错误、取消、页面导航和窗口关闭会释放未发布的写入器并删除临时文件。清理失败会报告状态，并在 Runtime 存活期间重试。指定 `expectedBytes` 时，提交要求长度完全一致。发布使用同文件系统原生重命名，Linux 可回退到不覆盖目标的硬链接发布。不支持相应发布操作的文件系统会返回错误。不保证断电后的持久性，进程崩溃可能留下临时目录，替换不保留原目标特有的权限或元数据。

发布操作是取消边界。已经发布成功时，取消返回 `committed`。丢失回复本身不能证明失败，可在文档存活期间查询状态或取消以确认结果。句柄不能跨窗口或重载使用，不支持续传、随机写入或崩溃恢复。

常见错误包括 `WRITE_BUSY`、`WRITER_INVALID`、`WRITER_CLOSED`、`WRITE_OFFSET_MISMATCH`、`WRITE_LENGTH_MISMATCH`、`BUFFER_LIMIT`、`FILE_EXISTS`、`ACCESS_DENIED`、`DISK_FULL` 和 `FILE_IO`。

## 浏览器辅助模块

现有三参数 `saveBlob(blob, filename, host?)` 调用保持兼容。可选第四参数提供取消和进度：

```js
import {saveBlob} from './browser-files.mjs';
const controller = new AbortController();
const path = await saveBlob(blob, 'capture.webm', reaper, {
  signal: controller.signal,
  onProgress: ({bytes, total, state}) => console.log(bytes, total, state),
});
```

辅助模块每次读取不超过 256 KiB 的 Blob 切片，并等待原生写入确认。进度包含已确认字节数及 `writing`、`committing`、`committed` 状态。取消保存对话框返回 `null`。取消信号返回 `WRITE_CANCELLED`，但已经提交的文件仍按成功处理。无法确认结果时返回 `WRITE_RESULT_UNKNOWN`。取消信号不能关闭已经打开的原生保存对话框。

显式安装的下载处理器流式读取已挂接到文档的 `blob:` 链接，在异步能力查询和保存对话框前取得响应。`data:` 链接与旧宿主继续使用 16 MiB 回退路径。同一宿主同时仅允许一次辅助导出，并发尝试返回 `WRITE_BUSY`。卸载处理器会取消未完成导出并移除监听，普通导航和未挂接的链接行为不变。

分块导出避免额外的整文件导出缓冲区，不改变 App 录制视频或构造 Blob 的方式。持续保留全部录制分块的录制器仍会占用随录制时长增长的内存。
