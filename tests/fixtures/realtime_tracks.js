(async () => {
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  async function until(check) {
    const end = performance.now() + 5000;
    while (!await check()) {
      if (performance.now() >= end) throw new Error('Timed out: ' + check);
      await wait(20);
    }
  }
  async function command(name) {
    await reaper.fs.writeText('fixture-command', name, {overwrite:true});
    await until(async () => {
      try {
        const reply = await reaper.fs.readText('fixture-' + name + '.txt');
        if (reply !== 'ok') throw new Error(name + ': ' + reply);
        return true;
      } catch (error) {
        if (error.code === 'FILE_NOT_FOUND') return false;
        throw error;
      }
    });
  }
  const output = {};
  async function phase(name) {
    output.phase = name;
    await reaper.fs.writeText('fixture-phase.json', JSON.stringify(output), {overwrite:true});
  }
  try {
    await window.demoReady;
    await phase('initial snapshot');
    const api = reaper.realtime;
    const ref = await reaper.audio.getSelectedTrackRef();
    const received = [];
    const errors = [];
    const sub = await api.subscribe('track-controls', {tracks:[ref.guid], updateRate:120,
      onData: state => received.push({time:performance.now(), track:state.tracks[0]}),
      onError: error => errors.push(error.code)});
    await until(() => received.length);
    await wait(250);
    if (received.length !== 1) throw new Error('Repeated unchanged snapshot');
    await phase('reorder');
    await command('reorder');
    const [volume, pan, mute, solo] = JSON.parse(await reaper.fs.readText('fixture-readback'));
    output.hostReadback = {guid:ref.guid, volume, pan, mute:!!mute, solo};
    if (volume !== 0.375 || pan !== -0.75 || !mute || !Number.isInteger(solo) || !solo)
      throw new Error('REAPER did not apply the requested track controls');
    await phase('compare host readback');
    await until(() => {
      const observed = output.snapshot = sub.latest()?.tracks[0];
      return observed && Object.keys(output.hostReadback).every(k => observed[k] === output.hostReadback[k]);
    });
    const firstId = (await api.getDiagnostics()).subscriptions.find(s => s.source === 'track-controls').id;
    for (const rate of [60,120]) {
      await phase('subscribe ' + rate);
      const times = [];
      let fieldsOK = true;
      const monitor = await api.subscribe('track-controls', {tracks:[ref.guid], properties:['volume'], updateRate:rate,
        onData: s => { fieldsOK &&= Object.keys(s.tracks[0]).length === 2; times.push(performance.now()); }});
      await until(() => times.length);
      await phase('volume changes ' + rate);
      const start = performance.now();
      const writes = [];
      const endVolume = rate === 60 ? 0.75 : 0.25;
      while (performance.now() - start < 1500) {
        writes.push(reaper.audio.setTrackValueLatest(ref,'D_VOL', (performance.now() - start) / 1500));
        await wait(12);
      }
      writes.push(reaper.audio.setTrackValueLatest(ref,'D_VOL',endVolume));
      await Promise.all(writes);
      await until(() => Math.abs(monitor.latest()?.tracks[0].volume - endVolume) < 1e-7);
      const diagnostic = (await api.getDiagnostics()).subscriptions.find(s => s.source === 'track-controls' && s.updateRate === rate && s.id !== firstId);
      output[rate] = {webHz:(times.length - 1) * 1000 / (performance.now() - start), diagnostic};
      if (!fieldsOK || times.length < 2 || !diagnostic) throw new Error('Missing volume updates or diagnostics at ' + rate);
      await monitor.close();
    }
    await phase('delete');
    await command('delete');
    await until(() => sub.closed);
    if (sub.latest() || !errors.includes('STALE_HANDLE')) throw new Error('Deleted track remained active');
    const next = await reaper.audio.getSelectedTrackRef();
    const switched = await api.subscribe('track-controls',{tracks:[next.guid]});
    await until(() => switched.latest());
    await phase('switch project');
    await command('switch');
    await until(() => switched.closed);
    if (switched.error?.code !== 'PROJECT_CHANGED') throw new Error('Missing project invalidation');
    output.diagnostics = await api.getDiagnostics();
    if (output.diagnostics.subscriptions.some(s => s.source === 'track-controls')) throw new Error('Subscription leak');
    output.passed = true;
  } catch (error) { output.error = String(error); output.stack = error.stack; output.passed = false; }
  await reaper.fs.writeText('result.json', JSON.stringify(output,null,2),{overwrite:true});
})();
