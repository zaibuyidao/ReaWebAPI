# ReaWebAPI v0.1.6

## Changes

- Preserve the complete 730-function REAPER 7.80 standard API mirror, typed native dispatch, Lua-order results, typed handles, binary MIDI and audio-buffer writeback. Third-party APIs and custom Lua RPC are outside this release.
- Fix Linux WebKit's 64 KiB request gate to honor the shared 64 MiB bridge limit.
- Reject foreign-project batch arguments before writes, keeping Undo attached to the project actually being edited.
- Expand synchronous batches to 173 reviewed methods / 128 calls, with references to earlier results and paired failure cleanup.
- Add managed Undo gestures with reload/close/project-change/30-second cleanup.
- Add worker-thread UTF-8/binary files, stat, directory listing/creation, native clipboard text and external links on all three backends.
- Add item selection, active-take selection, transport and FX invalidation events.
- Add an explicit loopback development entry, Vite/TypeScript starter and documented module/fetch/Worker conventions.
- Add Web Runtime v1: stable per-App loopback origins, native modules/local fetch, isolated persistent profiles and an unbundled browser conformance App. Keep old file-origin data untouched. Enable Linux persistent cookies and exclusive origin ports.
- Commit regression tests and make CI test each native build before publication.

## 中文说明

保持 REAPER 7.80 的 730 项标准 API 镜像绑定完整；本版不包含第三方 API 或自定义 Lua RPC。修复 Linux 大消息与跨工程批处理 Undo 两个缺陷，补充通用批处理/托管 Undo、文件和桌面服务、更多状态事件、现代前端模板。Web Runtime v1 补齐稳定本地来源、原生模块/fetch、按 App 隔离持久化存储与无需构建的能力检查示例。未添加项目许可证。

## Release acceptance

Automated coverage checks definitions, all 730 typed native call/return mappings against independent stubs, JS bridge behavior, runtime lifecycle, batch project ownership, file I/O, SDK types and packaging. These checks do not prove every REAPER API's effects on real projects.

Before distributing a platform binary, exercise it in that platform's REAPER: open/reload/dock/close, project switches with an Undo gesture, item/take selection, transport/FX updates, Unicode and binary files, clipboard and external links, a MIDI payload over 64 KiB, production Worker/resources and loopback HMR. Verify normal Undo/redo after MIDI, item and FX edits. macOS and both ARM64 binaries require their own CI and host acceptance; Windows/Linux mock or browser tests do not substitute for that.
