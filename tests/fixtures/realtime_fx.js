(async () => {
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  const result = {passed:false};
  async function until(check) {
    const end = performance.now() + 10000;
    while (performance.now() < end) {
      const value = await check();
      if (value) return value;
      await wait(25);
    }
    throw new Error('Timed out during ' + result.phase);
  }
  async function command(name) {
    result.phase = name;
    await reaper.fs.writeText('fx-command', name, {overwrite:true});
    const text = await until(async () => {
      try { return await reaper.fs.readText('fx-' + name + '.txt'); }
      catch (error) { if (error.code !== 'FILE_NOT_FOUND') throw error; }
    });
    try { return JSON.parse(text); } catch { throw new Error(text); }
  }
  try {
    await window.demoReady;
    const target = await command('init');
    const audio = reaper.audio;
    const ref = await audio.getFxRef(target);
    const reject = async (op, code) => {
      try { await op; } catch (e) { if (e.code === code) return; throw e; }
      throw new Error('Expected ' + code);
    };
    const initial = (await command('before-write'))[1];
    await reaper.transaction.withUndo('FX parameter review', async () => {
      const writes = await Promise.all([0.25,0.5,0.625].map(v => audio.setFxParameterLatest(ref,0,v)));
      if (!writes[0].applied || !writes[1].superseded || !writes[2].applied) throw new Error('FX write coalescing failed');
    });
    result.webWrite = (await command('after-write'))[1];
    result.undo = (await command('undo-write'))[1];
    result.redo = (await command('redo-write'))[1];
    if (result.webWrite !== 0.625 || Math.abs(result.undo-initial)>1e-7 || result.redo !== 0.625)
      throw new Error('FX write/undo/redo did not round trip');
    await reject(audio.setFxParameterLatest(ref, 0, 1.1),'INVALID_ARGUMENT');
    await reject(audio.setFxParameterLatest(ref, 99999, 0.5),'INVALID_ARGUMENT');
    const track = await reaper.realtime.subscribe('track-controls', {tracks:[target.track]});
    const observe = (binding, updateRate) => reaper.realtime.subscribe('fx-parameters',
      {...binding, parameters:[1,0], updateRate});
    const a = await observe(target, 60), b = await observe(target, 120);
    await until(() => a.latest() && b.latest() && track.latest());
    for (const step of ['set','reorder','bypass','enable','preset','automation-read','automation-seek-read']) {
      if (step === 'automation-read') {
        await command('automation');
        await wait(850);
      }
      if (step === 'automation-seek-read') {
        await command('automation-seek');
        await wait(250);
      }
      const expected = await command(step);
      if (step === 'preset' && expected === null) { result.preset = 'No installed preset'; continue; }
      if (step === 'automation-read' || step === 'automation-seek-read') {
        result.automationReachedEndpoint ||= Math.abs(expected[1] - 0.9) < 0.001;
        result.envelope = await reaper.fs.readText('fx-envelope.txt');
      }
      await until(() => [a,b].every(sub => {
        const s = sub.latest();
        return s?.fx === target.fx && s.track === target.track && s.values.length === 2 &&
          s.values.every((v,i) => v.index === 1-i && Math.abs(v.normalized - expected[i]) < 1e-10);
      }));
      result[step] = expected;
    }
    // The original index now holds another FX after the reorder.
    await command('finish-automation');
    await audio.setFxParameterLatest(ref,0,0.5625);
    await until(() => a.latest()?.values[1].normalized === 0.5625);
    result.afterReorder = (await command('reorder-write-read'))[1];
    if (result.afterReorder !== 0.5625) throw new Error('Write missed reordered FX');
    result.active = await reaper.realtime.getDiagnostics();
    await b.close();
    const replacement = await command('replace');
    await until(() => a.closed);
    if (replacement.fx === target.fx || a.error?.code !== 'STALE_HANDLE' || a.latest())
      throw new Error('Replaced FX binding remained valid');
    await reject(audio.setFxParameterLatest(ref,0,0.5),'STALE_HANDLE');
    const nextRef = await audio.getFxRef(replacement);
    const c = await observe(replacement, 60);
    await until(() => c.latest());
    result.chains = [];
    for (const row of await command('chains')) {
      const {expected, ...binding} = row;
      const watch = await reaper.realtime.subscribe('fx-parameters', {...binding, parameters:[0]});
      await until(() => watch.latest()?.values[0].normalized === expected);
      const writable = await audio.getFxRef(binding);
      await audio.setFxParameterLatest(writable,0,0.875);
      await until(() => watch.latest()?.values[0].normalized === 0.875);
      result.chains.push({chain:row.chain, track:row.track, initial:expected, written:0.875});
      await watch.close();
    }
    await command('switch');
    await until(() => c.closed && track.closed);
    if (c.error?.code !== 'PROJECT_CHANGED') throw new Error('Missing project invalidation');
    await reject(audio.setFxParameterLatest(nextRef,0,0.5),'PROJECT_CHANGED');
    result.final = await reaper.realtime.getDiagnostics();
    if (result.final.subscriptions.some(s => s.source === 'fx-parameters')) throw new Error('FX subscription leak');
    if (!result.automationReachedEndpoint) throw new Error('Host automation did not reach its programmed endpoint');
    result.passed = true;
  } catch (error) { result.error = String(error); result.stack = error.stack; }
  await reaper.fs.writeText('result.json', JSON.stringify(result,null,2), {overwrite:true});
})();
