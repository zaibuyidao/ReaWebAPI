# ReaWebAPI

## English

- Add explicit `instanceKey`, named-window `id` and `multiple` options to Lua `ReaWeb_Open`, with Runtime-managed reuse, focus and lifecycle cleanup.
- Update Lua launchers to pass their script source as the instance key. Calls without a key continue to create new windows.

## 简体中文

- Lua `ReaWeb_Open` 新增显式 `instanceKey`、命名窗口 `id` 和 `multiple` 参数，由 Runtime 管理实例复用、聚焦及生命周期清理。
- 更新 Lua 启动器，将脚本 source 作为实例 key。未传入 key 时继续每次创建新窗口。
