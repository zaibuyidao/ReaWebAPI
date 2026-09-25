# ReaWebAPI

## English

- Share `ReaWebAPI/WebViewData/` across Apps and Modules on Windows, macOS and Linux, with one native browser backend for active Apps.
- Keep App identities and origins unchanged. Report `storageIsolation: "origin"` for localStorage and IndexedDB isolation within the shared profile. Cookies on the same host are shared across ports.
- Store window state in `Apps/<appId>/WindowState/`, alongside private `Data/` and `origin.json`. Legacy browser data and global window state are not migrated.

## 简体中文

- Windows、macOS 和 Linux 的 App 与 Module 共享 `ReaWebAPI/WebViewData/`，活动 App 复用同一原生浏览器后端。
- 保留 App 身份和 origin 规则。`storageIsolation` 返回 `"origin"`，表示 localStorage 和 IndexedDB 在共享 profile 内按 origin 隔离。同一主机的不同端口共享 cookie。
- 窗口状态保存在 `Apps/<appId>/WindowState/`，与私有 `Data/` 和 `origin.json` 同级。旧浏览器数据和全局窗口状态不迁移。
