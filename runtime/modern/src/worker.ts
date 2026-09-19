// Web Workers can compute data; the bridge is only available in the top-level page.
self.onmessage = (event: MessageEvent<number[]>) => {
  self.postMessage(event.data.reduce((sum, value) => sum + value, 0));
};
