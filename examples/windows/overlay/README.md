# SVG overlay

[简体中文](README.zh-CN.md)

Requires ReaWebAPI v0.3.9.26 or later. Run `reawebapi-overlay.lua` in REAPER. English is the default on each load. Use the compact **EN** / **CN** buttons to switch languages.

The panel is centered. The initial window fits the English panel with 24 CSS pixels of space on each side, within the display work area. The decorative border follows the native window edges.

## Operation

- Drag the SVG knobs vertically to adjust the project's master gain and pan. Hold Shift for fine control. The pointer is hidden during mouse dragging and returns to its starting position on release. Esc or loss of focus ends the gesture without reverting parameter changes.
- **Interactive regions** receives mouse input on controls and passes other areas through to REAPER.
- **Window (move / resize)** receives input across the transparent window. Drag non-control areas to move it, or its native edges/corners to resize it. Controls retain their own interactions. Floating REAPER windows remain above the overlay in every mode.
- **Decoration only** passes all mouse input through. Register `restore-interaction.lua` and `close-overlay.lua` as REAPER actions before using this mode.
- The top-right **×** closes the window. **Background opacity** adjusts only the panel background (0–100%, default 40%), including in Interactive regions mode. Text, controls and outer decoration remain unchanged.
- **Fit display** reapplies the current display bounds.
- **Route shortcuts to REAPER Main** routes supported keys outside editable fields to Main bindings. Text and IME input stay in editable fields. System/browser-reserved shortcuts and arbitrary international keyboard layouts are outside the guarantee.

## Implementation

`index.html` contains the HTML, a labeled CSS block and a labeled JavaScript module. The module separates localization, SVG hit geometry, window policy, parameter gestures and cleanup. No separate JS/CSS files or build step are required.

`mouseRegions()` samples SVG fill/stroke at one CSS pixel resolution and merges the result into rectangles. DOM controls use bounding rectangles. Layout, resize, scroll and language changes refresh the regions. Each SVG scan is limited to 1,048,576 CSS pixels. Custom animations, style changes, clipping, masks and non-scaling strokes need application-specific updates.

Window mode enables the existing `data-reaweb-drag` contract on the body. Native exclusions protect buttons, inputs and SVG sliders. Other mouse modes remove the drag region. Resizing uses the host's existing frameless-window support.

Mouse gestures use the browser Pointer Lock API and relative movement. Release, cancellation and cleanup exit the lock. A denied lock reports an error and does not start parameter adjustment. Touch/pen input uses pointer capture. macOS and Linux authorize pointer lock only in standalone overlay windows.

## Platform conditions

- **Windows:** requires a WebView2 Runtime supporting native drag regions and transparent windows.
- **macOS:** before launching, open **System Settings → Privacy & Security → Accessibility** and enable **REAPER**. If absent, add the REAPER application with **+**. Quit and reopen REAPER after granting permission. Native mouse routing does not prompt automatically.
- **Linux:** requires native X11, X Shape, XInput, a compositor for transparency and a compatible window manager. Wayland/XWayland are unsupported. **Fit display** extends one native pixel beyond the bottom edge to avoid implicit fullscreen promotion. Keep this overscan when adapting the example. Native move/resize also requires `_NET_WM_MOVERESIZE` support.

See [overlay contracts](../../../docs/window-overlay.md) for API behavior, capabilities and errors.
