# ReaWebAPI

## Release Notes

- Fixed a startup error in the Native Transport Monitor when older HTML loads the newer script. The monitor can start with older layouts, and the updated page uses a versioned script URL.
- Lowered the macOS deployment target to 13. During WebView startup, Ventura uses WebKit's default persistent data store, avoiding the macOS 14-only API for opening a named store. macOS 14 and later continue to use the UUID-based store.
