# ReaWebAPI

- 普通 WebUI 未定义 app.json.id 时，在私有 .reawebapi/identity.json 标记中持久化随机 ID，不再依赖启动脚本名称。
- 移动与重启时保留自动身份，原绑定有效时为复制的 App 分配独立身份，初始化与更新串行执行。
- 替换 launcher 派生的回退身份，不迁移其存储，旧数据继续保留。显式 ID、JSFX 身份派生和窗口实例规则保持不变。
