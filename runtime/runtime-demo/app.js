/// <reference path="../reaper.d.ts" />
const el = id => document.getElementById(id);
let waveform = null;
let selectedAudioPath = null, appName = 'Runtime Studio';
let trackRevision = 0;
const showError = error => { el('status').textContent = `${error.code || 'ERROR'}: ${error.message}`; };
const action = (id, fn) => el(id).addEventListener('click', () => Promise.resolve().then(fn).catch(showError));
function draw() {
  const canvas = el('wave'), context = canvas.getContext('2d');
  context.clearRect(0, 0, canvas.width, canvas.height);
  if (!waveform) return;
  context.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue('--reaper-highlight').trim();
  const height = canvas.height / waveform.channels;
  for (let channel = 0; channel < waveform.channels; ++channel) {
    const { min, max } = waveform.data[channel];
    context.beginPath();
    for (let i = 0; i < waveform.points; ++i) {
      const x = i * canvas.width / waveform.points, center = height * (channel + 0.5);
      context.moveTo(x, center - max[i] * height * 0.45); context.lineTo(x, center - min[i] * height * 0.45);
    }
    context.stroke();
  }
}
async function selectedTrack() {
  const revision = ++trackRevision;
  const track = await reaper.GetSelectedTrack(0, 0);
  const name = track ? (await reaper.GetTrackName(track))[1] : 'No track selected';
  if (revision === trackRevision) el('track').textContent = name;
  return track;
}
action('open', async () => {
  const path = await reaper.dialog.openFile({title:'Choose audio',filters:[{name:'Audio',extensions:['wav','aiff','flac','mp3','ogg']},{name:'All files',extensions:['*']}]});
  if (!path) return;
  el('open').disabled = true;
  try {
    const info = await reaper.audio.getFileInfo(path);
    selectedAudioPath = info.path; el('drag-file').disabled = false;
    el('file').textContent = `${info.path}\n${info.sampleRate} Hz · ${info.channels} channels · ${info.bitDepth ?? 'unknown'} bit · ${info.duration.toFixed(2)} seconds`;
    el('status').textContent = 'Reading waveform…';
    waveform = await reaper.audio.getWaveform(path, {points:1200}); draw();
    el('export').disabled = false; el('status').textContent = `Read ${waveform.points} peak frames per channel.`;
  } finally { el('open').disabled = false; }
});
action('export', async () => {
  if (!waveform) return;
  const path = await reaper.dialog.saveFile({title:'Export waveform',initialPath:'waveform.json',filters:[{name:'JSON',extensions:['json']}]});
  if (path) { await reaper.fs.writeFile(path, JSON.stringify(waveform), {overwrite:true}); el('status').textContent = `Saved ${path}`; }
});
action('meter', async () => {
  const track = await selectedTrack();
  el('levels').textContent = track ? JSON.stringify(await reaper.audio.getTrackMeter(track), null, 2) : 'Select a track first.';
});
action('resize', () => reaper.window.setSize(900,760));
action('dock', async () => (await reaper.window.getState()).docked ? reaper.window.undock() : reaper.window.dock());
action('reload', () => reaper.window.reload());
action('diagnostics', async () => { el('status').textContent = JSON.stringify(await reaper.debug.getDiagnostics(),null,2); });
action('data-folder', async () => reaper.system.revealInFileManager(await reaper.app.getDataPath()));
for (const [id, start] of [['drag-file', () => reaper.dragDrop.startFiles([selectedAudioPath])],
  ['drag-text', () => reaper.dragDrop.startText(appName)]]) {
  el(id).addEventListener('pointerdown', event => {
    if (event.button !== 0) return;
    event.preventDefault();
    start().then(accepted => { el('drop').textContent = accepted ? 'Copy accepted by the target.' : 'Drag cancelled or rejected.'; }).catch(showError);
  });
}
try {
  const capabilities = await reaper.lifecycle.ready;
  appName = await reaper.app.getName();
  el('app-info').textContent = JSON.stringify({id:await reaper.app.getId(),name:appName,version:await reaper.app.getVersion(),
    rootPath:await reaper.app.getRootPath(),dataPath:await reaper.app.getDataPath(),
    platform:await reaper.system.getPlatform(),architecture:await reaper.system.getArchitecture()},null,2);
  await reaper.dragDrop.onDrop(payload => { el('drop').textContent = JSON.stringify(payload,null,2); });
  await reaper.theme.apply();
  await reaper.theme.onChange(draw);
  await selectedTrack();
  await reaper.events.on('track-selected', async () => { await selectedTrack(); });
  for (const name of ['track-added','track-deleted','item-changed','take-changed','playback-state-changed','tempo-changed','marker-changed','fx-changed','project-loaded','project-saved'])
    await reaper.events.on(name, event => { el('event').textContent = `${name}: ${JSON.stringify(event)}`; });
  await reaper.lifecycle.on('cleanup', event => { localStorage.setItem('lastCleanup', JSON.stringify({reason:event.reason,time:Date.now()})); });
  el('status').textContent = `Connected to ReaWebAPI ${capabilities.version}. Previous cleanup: ${localStorage.getItem('lastCleanup') || 'none'}`;
} catch (error) { showError(error); }
