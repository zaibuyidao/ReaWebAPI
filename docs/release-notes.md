# ReaWebAPI

- Reduce Linux multi-window latency by reading WebKit input on arrival and caching X11 geometry until native window changes.
- Reduce GTK drawing stalls in WSL without DRI3 using image backing surfaces in the helper. Preserve explicit GDK_RENDERING settings.
