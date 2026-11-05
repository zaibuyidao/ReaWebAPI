'use strict';
(async () => {
  const status = document.querySelector('#status');
  const fail = error => { status.textContent = `${error.code || 'ERROR'}: ${error.message}`; };
  try {
    await reaper.lifecycle.ready;
    await reaper.window.setKeyboardCapture(false);
    await reaper.window.setDawKeyboard(true);
    let scheduled = false, disposed = false;
    const update = () => {
      if (scheduled || disposed) return;
      scheduled = true;
      requestAnimationFrame(() => {
        scheduled = false;
        if (disposed) return;
        const regions = [...document.querySelectorAll('.interactive')].map(element => element.getBoundingClientRect())
          .filter(rect => rect.width > 0 && rect.height > 0)
          .map(({x, y, width, height}) => ({x, y, width, height}));
        reaper.window.setMouseRegions(regions).catch(fail);
      });
    };
    const resize = new ResizeObserver(update);
    document.querySelectorAll('.interactive').forEach(element => resize.observe(element));
    const layout = new MutationObserver(update);
    layout.observe(document.querySelector('main'), {attributes: true, childList: true, subtree: true});
    addEventListener('resize', update);
    addEventListener('scroll', update, true);
    const show = state => {
      document.querySelector('#mode').value = state.mouseMode;
      status.textContent = JSON.stringify({layer: state.layer, layerActive: state.layerActive,
        mouseMode: state.mouseMode, mouseModeActive: state.mouseModeActive}, null, 2);
    };
    await reaper.events.on('windowstatechange', show);
    show(await reaper.window.getState());
    update();
    const bind = (selector, event, action) => document.querySelector(selector).addEventListener(event,
      e => Promise.resolve().then(() => action(e)).catch(fail));
    bind('#mode', 'change', event => reaper.window.setMouseMode(event.target.value));
    bind('#capture', 'change', event => reaper.window.setKeyboardCapture(event.target.checked));
    bind('#space', 'change', event => reaper.window.setDawKeyboard(event.target.checked));
    bind('#play', 'click', () => reaper.Main_OnCommand(40044, 0));
    bind('#reload', 'click', () => reaper.window.reload());
    bind('#close', 'click', () => reaper.window.close());
    let count = 0;
    bind('#count', 'click', event => { event.target.textContent = `Transparent control: ${++count}`; update(); });
    bind('#level', 'input', event => { document.querySelector('#value').value = event.target.value; });
    await reaper.lifecycle.on('cleanup', () => {
      disposed = true; resize.disconnect(); layout.disconnect();
      removeEventListener('resize', update); removeEventListener('scroll', update, true);
    });
  } catch (error) { fail(error); }
})();
