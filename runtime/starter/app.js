// The extension injects reaper. Do not load a separate bridge script.
(() => {
  'use strict';
  const label = document.getElementById('track-name');
  const status = document.getElementById('status');
  const refreshButton = document.getElementById('refresh');
  const dockButton = document.getElementById('dock');
  if (!label || !status || !(refreshButton instanceof HTMLButtonElement) || !(dockButton instanceof HTMLButtonElement)) return;

  /** @param {unknown} error */
  const showError = error => {
    console.error(error);
    status.textContent = error instanceof Error ? error.message : String(error);
  };
  let busy = false, requested = false;
  const refresh = async () => {
    requested = true;
    if (busy) return;
    busy = true;
    try {
      do {
        requested = false;
        const track = await reaper.GetSelectedTrack(0, 0);
        const name = track ? await reaper.GetTrackName(track) : null;
        label.textContent = name?.[0] ? name[1] : 'No track selected';
      } while (requested);
    } catch (error) {
      label.textContent = 'Refresh to read the current project';
      showError(error);
    } finally {
      busy = false;
    }
  };

  const start = async () => {
    if (typeof window.reaper === 'undefined') {
      status.textContent = 'Open this page with Open.lua inside REAPER.';
      label.textContent = 'REAPER connection required';
      return;
    }
    const { api } = await reaper.lifecycle.ready;
    if (!api || !Array.isArray(api.availableMethods)) {
      throw new Error('Install ReaWebAPI 0.1.3 or newer and reopen this page.');
    }
    if (!['GetSelectedTrack', 'GetTrackName'].every(name => api.availableMethods.includes(name))) {
      throw new Error('This REAPER installation is missing a required API.');
    }
    await reaper.window.setTitle('ReaWebAPI Starter');
    await reaper.events.on('windowstatechange', state => {
      dockButton.textContent = state.docked ? 'Undock' : 'Dock';
    });
    // Refresh catches its own asynchronous errors. Initial snapshots also refresh.
    await reaper.events.on('selectionchange', () => { void refresh(); });
    await reaper.events.on('projectchange', () => { void refresh(); });
    refreshButton.addEventListener('click', () => { void refresh(); });
    dockButton.addEventListener('click', async () => {
      dockButton.disabled = true;
      try { await reaper.window.setDocked(!await reaper.window.isDocked()); }
      catch (error) { showError(error); }
      finally { dockButton.disabled = false; }
    });
    refreshButton.disabled = dockButton.disabled = false;
    status.textContent = `${api.available} REAPER APIs available. Edit app.js to begin.`;
  };
  void start().catch(showError);
})();
