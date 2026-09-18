'use strict';
const ui = Object.fromEntries(['status', 'track-count', 'track-name', 'read-track', 'devtools', 'dock', 'activity', 'error', 'version', 'pan', 'pan-value', 'center-pan', 'diagnostics', 'diagnostic-output'].map(id => [id, document.getElementById(id)]));
const events = [], dispose = [];
let selectedTrack = null, refreshWanted = false, refreshing = false, dockBusy = false;
let pendingPan = Promise.resolve();
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
  try { await action(); } catch (error) { report(error); }
}
function showPan(value) {
  ui.pan.value = value;
  ui['pan-value'].textContent = Number(value).toFixed(2);
}
async function refresh() {
  refreshWanted = true;
  if (refreshing) return;
  refreshing = true;
  try {
    do {
      refreshWanted = false;
      const track = await reaper.GetSelectedTrack(0, 0);
      const calls = [{ method: 'CountTracks', args: [0] }];
      if (track) calls.push({ method: 'GetTrackName', args: [track] }, { method: 'GetMediaTrackInfo_Value', args: [track, 'D_PAN'] });
      const [count, name, pan] = await reaper.ReaWeb_Batch(calls);
      if (refreshWanted) continue;
      selectedTrack = track;
      ui['track-name'].textContent = name ?? 'No track selected.';
      ui['track-count'].textContent = `${count} ${count === 1 ? 'track' : 'tracks'} in project`;
      ui.pan.disabled = ui['center-pan'].disabled = !track;
      if (document.activeElement !== ui.pan) showPan(pan ?? 0);
    } while (refreshWanted);
  } catch (error) {
    selectedTrack = null;
    ui.pan.disabled = ui['center-pan'].disabled = true;
    // Project/selection events schedule the next read. Writes are never retried.
    if (!['PROJECT_CHANGED', 'STALE_HANDLE', 'WINDOW_CLOSED'].includes(error.code)) report(error);
  } finally {
    refreshing = false;
    if (refreshWanted) void refresh();
  }
}
ui['read-track'].addEventListener('click', () => run(async () => { await refresh(); log('Track data refreshed.'); }));
ui.devtools.addEventListener('click', () => run(async () => { await reaper.ReaWeb_DevTools(); log('Developer Tools opened.'); }));
function showDockState(docked) {
  ui.dock.textContent = docked ? 'Undock' : 'Dock';
  ui.dock.setAttribute('aria-pressed', String(docked));
}
ui.dock.addEventListener('click', () => run(async () => {
  if (dockBusy) return;
  dockBusy = true; ui.dock.disabled = true;
  try {
    const docked = await reaper.ReaWeb_SetDocked(!(await reaper.ReaWeb_IsDocked()));
    showDockState(docked); log(docked ? 'Window docked in REAPER.' : 'Window undocked.');
  } finally { dockBusy = false; ui.dock.disabled = false; }
}));
ui.pan.addEventListener('input', () => {
  const track = selectedTrack, value = Number(ui.pan.value);
  ui['pan-value'].textContent = value.toFixed(2);
  if (track) pendingPan = reaper.ReaWeb_SetTrackValueLatest(track, 'D_PAN', value).catch(report);
});
ui['center-pan'].addEventListener('click', () => run(async () => {
  if (!selectedTrack) return;
  const track = selectedTrack;
  ui.pan.disabled = true;
  try {
    await pendingPan;
    await reaper.ReaWeb_Batch([{ method: 'SetMediaTrackInfo_Value', args: [track, 'D_PAN', 0] }], { undoLabel: 'ReaWebAPI: center track pan' });
    showPan(0); log('Pan centered. Undo is available in REAPER.');
  } finally { ui.pan.disabled = !selectedTrack; }
}));
ui.diagnostics.addEventListener('click', () => run(async () => {
  ui['diagnostic-output'].textContent = JSON.stringify(await reaper.ReaWeb_GetDiagnostics(), null, 2);
  ui['diagnostic-output'].hidden = false;
}));
window.addEventListener('pagehide', () => { for (const off of dispose) void off(); });
run(async () => {
  if (!window.reaper) {
    ui.status.textContent = 'Outside REAPER';
    for (const control of ['read-track', 'devtools', 'dock', 'diagnostics']) ui[control].disabled = true;
    throw new Error('Load Example.lua from the REAPER Action List to open this demo.');
  }
  const capabilities = await reaper.ready;
  const version = await reaper.GetAppVersion();
  ui.status.textContent = 'Runtime connected'; ui.status.classList.add('connected');
  ui.version.textContent = `REAPER ${version} / ReaWebAPI ${capabilities.version}`;
  await reaper.ReaWeb_SetTitle('ReaWebAPI · Track Inspector');
  dispose.push(await reaper.ReaWeb_On('windowstatechange', state => showDockState(state.docked)));
  dispose.push(await reaper.ReaWeb_On('selectionchange', () => { selectedTrack = null; void refresh(); }));
  dispose.push(await reaper.ReaWeb_On('projectchange', () => { void refresh(); }));
  ui.dock.disabled = false;
  await refresh();
  log('Connected. Selection and project changes update this page automatically.');
});
