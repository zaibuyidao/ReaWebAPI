# SVG overlay

Requires ReaWebAPI v0.3.9.25 or later. Run `reawebapi-overlay.lua` in REAPER. The window fits its current display and stays above REAPER's main window, below floating REAPER windows.

- Drag the SVG knobs vertically to change the actual master gain and pan. Shift gives finer control. Pointer capture completes a drag outside the circle.
- Decorations and gaps pass mouse input to native REAPER controls. Circle corners outside the SVG shape also pass through.
- Main-section shortcuts are enabled outside editable fields. Click the text field for text/IME input. Disable **Route shortcuts** to restore page keyboard behavior.
- **Decoration only** passes all mouse input through. Run `restore-interaction.lua` or `close-overlay.lua` from REAPER to recover.
- **Fit display** reapplies the display bounds after moving the window or changing the display layout.

`regions.js` converts SVG geometry fill/stroke into rectangles at one CSS pixel resolution. Ordinary DOM controls use bounding rectangles. Resize, scroll and geometry changes republish the regions. Custom animations, style changes, clipping, masks and non-scaling strokes require application-specific region updates. The helper limits each SVG scan to 1,048,576 CSS pixels. This example targets small controls, not rasterization of a full-screen illustration.

On Linux, **Fit display** extends one native pixel past the screen edge to prevent implicit fullscreen promotion by the window manager. Keep this overscan when adapting the example.

macOS requires REAPER Accessibility permission for native mouse routing. Linux requires a native X11 session, X Shape, XInput and a compatible window manager. See [overlay contracts](../../../docs/window-overlay.md).

## 简体中文

运行 `reawebapi-overlay.lua`。示例覆盖当前显示器，提供 SVG 装饰、主音量和声像旋钮。纵向拖动旋钮，按住 Shift 可精细调整。拖出圆形区域后仍可释放。装饰和控件空隙允许鼠标操作到达 REAPER。

默认启用 REAPER Main 快捷键路由。文本框内保留文字和 IME 输入。选择 Decoration only 后，通过 REAPER 运行 `restore-interaction.lua` 恢复交互，或运行 `close-overlay.lua` 关闭覆盖层。

SVG 区域按 1 CSS 像素精度近似。普通 DOM 控件使用包围矩形。自定义动画、样式、裁剪和遮罩需要 App 自行维护区域。示例会直接修改当前工程的主音量和声像。

Linux 的 Fit display 向屏幕外多铺 1 个原生像素，避免窗口被自动提升到全屏层。修改示例时需保留此处理。
