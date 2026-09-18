'use strict';
const ui = Object.fromEntries(['status', 'track-count', 'track-name', 'read-track', 'devtools', 'dock', 'activity', 'error', 'version'].map(id => [id, document.getElementById(id)]));
const events = [];
function log(message) {
  const time = new Date().toLocaleTimeString([], { hour12: false });
  events.unshift(`${time}  ${message}`);
  ui.activity.textContent = events.slice(0, 5).join('\n');
  console.log('[ReaWebAPI demo]', message);
}
function report(error) {
  ui.error.hidden = false;
  ui.error.textContent = `${error.code || 'ERROR'}: ${error.message}`;
  log(error.message);
  console.error('[ReaWebAPI demo]', error);
}
async function run(action) {
  ui.error.hidden = true;
  ui['read-track'].disabled = true;
  try { await action(); } catch (error) { report(error); }
  finally { ui['read-track'].disabled = !window.reaper; }
}
ui['read-track'].addEventListener('click', () => run(async () => {
  const track = await reaper.GetSelectedTrack(0, 0);
  if (!track) {
    ui['track-name'].textContent = 'No track selected.';
    log('Select a track in REAPER and try again.');
  } else {
    const name = await reaper.GetTrackName(track);
    ui['track-name'].textContent = name;
    log(`Selected Track: ${name}`);
  }
  const count = await reaper.CountTracks(0);
  ui['track-count'].textContent = `${count} ${count === 1 ? 'track' : 'tracks'} in project`;
}));
ui.devtools.addEventListener('click', () => run(async () => {
  await reaper.ReaWeb_DevTools();
  log('Developer Tools opened. Check the Console tab.');
}));
function showDockState(docked) {
  ui.dock.textContent = docked ? 'Undock' : 'Dock';
  ui.dock.setAttribute('aria-pressed', String(docked));
}
let dockBusy = false;
ui.dock.addEventListener('click', async () => {
  if (dockBusy) return;
  dockBusy = true;
  ui.dock.disabled = true;
  ui.error.hidden = true;
  try {
    const docked = await reaper.ReaWeb_SetDocked(!(await reaper.ReaWeb_IsDocked()));
    showDockState(docked);
    log(docked ? 'Window docked in REAPER.' : 'Window undocked.');
  } catch (error) { report(error); }
  finally { dockBusy = false; ui.dock.disabled = false; }
});
// REAPER can also move a Docker tab through its own menus.
const dockTimer = setInterval(async () => {
  if (!window.reaper || document.hidden || dockBusy || ui.dock.disabled) return;
  dockBusy = true;
  try { showDockState(await reaper.ReaWeb_IsDocked()); }
  catch (error) { report(error); clearInterval(dockTimer); }
  finally { dockBusy = false; }
}, 1000);
window.addEventListener('pagehide', () => clearInterval(dockTimer));
run(async () => {
  if (!window.reaper) {
    ui.status.textContent = 'Outside REAPER';
    ui.devtools.disabled = true;
    throw new Error('Load Example.lua from the REAPER Action List to open this demo.');
  }
  const [version, count, capabilities] = await Promise.all([
    reaper.GetAppVersion(), reaper.CountTracks(0), reaper.ReaWeb_GetCapabilities()
  ]);
  ui.status.textContent = 'Runtime connected';
  ui.status.classList.add('connected');
  ui['track-count'].textContent = `${count} ${count === 1 ? 'track' : 'tracks'} in project`;
  ui.version.textContent = `REAPER ${version} / ReaWebAPI ${capabilities.version}`;
  showDockState(await reaper.ReaWeb_IsDocked());
  ui.dock.disabled = false;
  log(`Connected · ${capabilities.methods.length} APIs available`);
  log('Select a track, then press the button.');
});
