import './style.css';
import ComputeWorker from './worker?worker&inline';

const el = (id: string) => document.getElementById(id)!;
const output = (value: unknown) => { el('output').textContent = typeof value === 'string' ? value : JSON.stringify(value, null, 2); };
const report = (error: unknown) => { el('status').textContent = error instanceof Error ? error.message : String(error); };
let projectName = '';
let worker: Worker | undefined;
const disposers: Array<() => Promise<void>> = [];
function action(id: string, run: () => Promise<unknown>) {
  const button = el(id) as HTMLButtonElement;
  button.onclick = async () => {
    button.disabled = true;
    try { output(await run()); el('status').textContent = 'Ready'; } catch (error) { report(error); }
    finally { button.disabled = false; }
  };
}
async function main() {
  if (!window.reaper) throw new Error('Open this page with Open.lua or OpenDev.lua in REAPER.');
  const r = window.reaper;
  await r.lifecycle.ready;
  projectName = await r.GetProjectName(0);
  el('project').textContent = (projectName || 'Unsaved project') + ' · ' + await r.CountTracks(0) + ' tracks';
  action('gain', async () => {
    const track = await r.GetSelectedTrack(0, 0);
    if (!track) throw new Error('Select a track first.');
    return r.transaction.batch(b => {
      b.SetMediaTrackInfo_Value(track, 'D_VOL', Math.pow(10, -6 / 20));
      const [, name] = b.GetTrackName(track);
      return { name, volume: b.GetMediaTrackInfo_Value(track, 'D_VOL') };
    }, { undoLabel: 'Set track to −6 dB' });
  });
  action('save', () => r.fs.writeFile('snapshot.json', JSON.stringify({ projectName, saved: new Date().toISOString() }, null, 2), { overwrite: true }));
  action('read', async () => JSON.parse(await r.fs.readFile('snapshot.json')));
  action('copy', () => r.clipboard.writeText(projectName));
  action('paste', () => r.clipboard.readText());
  action('docs', () => r.system.openExternal('https://www.reaper.fm/sdk/reascript/reascripthelp.html'));
  disposers.push(await r.events.on('transportchange', state => {
    if (!state.available) { el('transport').textContent = 'Transport state unavailable'; return; }
    el('transport').textContent = 'State ' + state.state + ' · ' + (state.position ?? 0).toFixed(2) + ' s · ' + state.tempo + ' BPM';
  }));
  disposers.push(await r.events.on('projectchange', () => { el('status').textContent = 'Project changed. Reload this example to refresh the project info.'; }));
  const response = await fetch('./data.json');
  if (!response.ok) throw new Error('Resource request failed');
  const data = await response.json();
  worker = new ComputeWorker();
  worker.onmessage = event => { el('resources').textContent = data.label + ' · Worker sum: ' + event.data; };
  worker.onerror = () => { el('resources').textContent = 'Worker failed to start in this WebView.'; };
  worker.postMessage([1, 2, 3, 4]);
  el('status').textContent = 'Ready';
}
window.addEventListener('pagehide', () => { worker?.terminate(); for (const dispose of disposers) void dispose().catch(() => {}); });
void main().catch(report);
