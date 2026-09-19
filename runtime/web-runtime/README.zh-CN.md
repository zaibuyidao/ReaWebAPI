# Web Runtime 能力检查

[English](README.md) | **简体中文**

需要 ReaWebAPI v0.1.6。在 REAPER 运行 `Open.lua`，无需构建、npm 或导入桥接。请保留完整目录结构。

页面直接导入模块、fetch 本地 JSON，检查必需和可选浏览器能力。重开后 localStorage、cookie、IndexedDB 访问计数应增加；复制到另一个目录后应从 1 开始，以检查隔离。将文本或实体文件拖入区域可人工验收系统集成；自动检查仅派发 DOM 事件。

只写入本示例自己的浏览器存储 key/数据库，读取 REAPER 版本和能力，不修改工程。WebSocket 构造器检查仅表示存在；自动原生宿主测试另提供本地 HTTP/WebSocket 服务，验证实际收发。本示例不访问外部服务。

范围、存储路径、迁移及平台要求见 [Web Runtime 约定](../../docs/frontend.zh-CN.md)。检查动画帧时请保持窗口可见。
