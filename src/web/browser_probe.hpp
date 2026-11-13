#pragma once
namespace reaweb {
inline constexpr char browser_probe_script[] = R"JS(
'use strict';
onmessage = ({ data }) => {
  const result = { token: data.token, crossOriginIsolated: self.crossOriginIsolated === true,
    sharedArrayBuffer: false, wasmSharedMemory: false, sharedRoundTrip: null };
  try {
    const view = new Int32Array(new SharedArrayBuffer(4));
    Atomics.store(view, 0, 7);
    result.sharedArrayBuffer = Atomics.load(view, 0) === 7;
  } catch {}
  try { result.wasmSharedMemory = new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true }).buffer instanceof SharedArrayBuffer; } catch {}
  if (data.memory) {
    try { result.sharedRoundTrip = Atomics.add(new Int32Array(data.memory.buffer), 0, 1) === 17; }
    catch { result.sharedRoundTrip = false; }
  }
  postMessage(result);
};
)JS";
}
