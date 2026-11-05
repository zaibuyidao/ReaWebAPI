"""JS/Lua overlay lifecycle acceptance in an isolated REAPER profile."""
import transparent_smoke

smoke = transparent_smoke.hidden
smoke.SCRIPT = smoke.SCRIPT.replace('"transparent":true}', '"transparent":true,"layer":"overlay","mouseMode":"regions"}')
smoke.SCRIPT = smoke.SCRIPT.replace("check(reaper.ReaWeb_Show(id));", """
    check(reaper.ReaWeb_SetMouseRegions(id, '[{"x":20,"y":20,"width":80,"height":80}]'))
    check(reaper.ReaWeb_SetMouseMode(id, 'passthrough'))
    check(reaper.ReaWeb_SetMouseMode(id, 'normal'))
    check(not reaper.ReaWeb_SetMouseRegions(id, '[{"x":0,"y":0,"width":0,"height":1}]'))
    check(reaper.ReaWeb_GetLastError():find('positive'))
    check(reaper.ReaWeb_Show(id));""")
smoke.PAGE = smoke.PAGE.replace('  await reaper.lifecycle.ready;', """
  await reaper.lifecycle.ready;
  const features = (await reaper.system.getCapabilities()).runtime.window.overlay;
  if (!features.layer.supported || !features.regions.supported || !features.combined.supported)
    throw Error('Unsupported native policy: ' + JSON.stringify(features));
  for (const mode of ['normal', 'regions', 'passthrough', 'regions']) {
    if (await reaper.window.setMouseMode(mode) !== true) throw Error('Mouse mode result');
  }
  await reaper.window.setMouseRegions([{x:-20,y:10,width:120,height:90}]);
  let rejected = false;
  try { await reaper.window.setMouseRegions([{x:0,y:0,width:0,height:4}]); }
  catch (e) { rejected = e.code === 'INVALID_ARGUMENT'; }
  if (!rejected) throw Error('Invalid regions accepted');
""")
smoke.PAGE = smoke.PAGE.replace('if (!state.transparent ||', "if (state.layer !== 'overlay' || (state.visible && !state.layerActive)) throw Error('Layer state: ' + JSON.stringify(state));\n      if (!state.transparent ||")
smoke.PAGE = smoke.PAGE.replace('n < 500 && !state.visible', 'n < 500 && (!state.visible || !state.layerActive)')
smoke.SCRIPT = smoke.SCRIPT.replace('transparent hidden opens, state, Lua/JS Dock rejection, Show/Focus, reuse, reload and close',
    'overlay opens, JS/Lua mouse modes and regions, invalid inputs, Dock rejection, Show/Focus, reuse, reload and close')

if __name__ == '__main__':
    raise SystemExit(smoke.main())
