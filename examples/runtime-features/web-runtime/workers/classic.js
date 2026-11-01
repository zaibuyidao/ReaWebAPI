onmessage = async () => {
  try { postMessage({ value: (await (await fetch('../data/config.json')).json()).value }); }
  catch (error) { postMessage({ error: String(error) }); }
};
