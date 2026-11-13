# WebAssembly threads

Run `reawebapi-isolation.lua` inside REAPER. The manifest requires cross-origin isolation. Close all windows of this App before changing the policy.

The page reports actual browser capabilities and runs four URL Workers followed by four Blob Workers. Each group shares a WASM memory and a SAB, waits/notifies with Atomics and executes `atomic.wasm` to produce 100000. `atomic.wat` is its source. The fixture also exercises Worker dependencies, exceptions, self-exit and persistent localStorage/IndexedDB counters. The App terminates Workers and revokes its Blob URLs on completion.

`webgl2` is reported independently and is not required for this computation. The example does not certify Emscripten core compatibility or audio/rendering behavior. See the [Web Runtime contract](../../../docs/frontend.md#app-level-cross-origin-isolation).
