# ReaWebAPI

## English

- Add native macOS WKWebView Web Inspector with right-hand Embedded and native Floating modes, without an additional window frame.
- Add state-aware Open/Hide/Float/Embed context-menu actions and Option+Command+I. Reuse the existing Runtime API and saved layout preferences, retaining the Inspector session across hiding and mode switches.
- Detect WebKit Inspector capabilities at runtime and report native floating fallback when embedding is unavailable.

## 简体中文

- macOS WKWebView 新增原生 Web Inspector，支持右侧 Embedded 和原生 Floating 模式，不增加外层窗口边框。
- 新增随状态变化的 Open/Hide/Float/Embed 右键菜单和 Option+Command+I，复用现有 Runtime API 与布局偏好，隐藏及切换模式时保留检查器会话。
- 运行时检查 WebKit Inspector 能力，嵌入不可用时回退到原生浮动窗口并提供诊断。
