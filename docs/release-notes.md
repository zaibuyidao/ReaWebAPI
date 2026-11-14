# ReaWebAPI

- Use system video-processor scaling for supported Windows native I420 previews, retaining shader fallback.
- Reduce video-preview aliasing when downscaling in Windows native shader fallback, RGBA WebGL rendering and RGBA fit/custom output.
- Redraw RGBA web previews at physical display dimensions during resizing, including high-quality Canvas 2D fallback.
