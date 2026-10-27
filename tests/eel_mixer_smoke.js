(async () => {
  const el = id => document.getElementById(id);
  const pause = () => new Promise(resolve => setTimeout(resolve, 50));
  const wait = async predicate => { for (let i=0;i<400;i++) { if (await predicate()) return; await pause(); } throw Error('page timeout'); };
  const mark = value => reaper.SetExtState('EEL.Mixer.Test','stage',value,false);
  const next = value => reaper.GetExtState('EEL.Mixer.Test','next').then(text => text===value);
  const slider = (id,value) => { el(id).value=value;el(id).dispatchEvent(new Event('input'));el(id).dispatchEvent(new Event('change')); };
  let phase='connect', lastMessage='', supersededSeen=false, framesSeen=0, messageStates=0;
  try {
    await reaper.events.on('message', message => { lastMessage=message; if(message.startsWith('state ')) messageStates++; if(message.startsWith('ack ') && message.includes(' superseded ')) supersededSeen=true; });
    await reaper.events.on('frame', frame => { if (frame.lua?.startsWith('state ')) framesSeen++; });
    await wait(() => !el('controls').disabled && framesSeen > 0);
    if (el('name').textContent!=='鼓组 "A" \\ <b>🎛</b>\n\tend' || el('name').children.length) throw Error('Unsafe or incorrect track name');
    const track=await reaper.GetSelectedTrack(0,0);
    if (sessionStorage.reloading) {
      await wait(() => el('volume').value === '3.5');
      await mark('reloaded');
      await wait(() => el('controls').disabled && el('name').textContent === 'No track selected');
      await mark('none'); return;
    }
    phase='edit';
    slider('volume',0); await drainSlider('volume');
    if (await reaper.GetMediaTrackInfo_Value(track,'D_VOL') !== 0) throw Error('Zero gain failed');
    for (const value of [-.1, 4.1]) {
      let invalid = false;
      try { await request('volume',value); } catch(error) { invalid = error.message.includes('invalid'); }
      if (!invalid || await reaper.GetMediaTrackInfo_Value(track,'D_VOL') !== 0) throw Error('Out-of-range gain accepted');
    }
    const oldState = state;
    const oldMessage = `state ${session} ${state.revision} ${state.context} ${state.guid} ${state.volume} ${state.pan} 0 0 0 0 0 0\nold snapshot`;
    for(let i=0;i<500;i++) { slider('volume',(i%100)/100);slider('pan',(i%100)/100); }
    if(sliders.volume.flights.size!==8 || sliders.pan.flights.size!==8 || !sliders.volume.queued || !sliders.pan.queued) throw Error('Execution slots released before ack');
    slider('volume',4);slider('pan',-.37);
    let drained=false;
    const draining=Promise.all([drainSlider('volume'),drainSlider('pan')]).then(()=>{drained=true;});
    if(drained) throw Error('Final drain resolved before execution acknowledgement');
    onFrame({lua:oldMessage}); await new Promise(requestAnimationFrame);
    if(el('volume').value!=='4' || el('pan').value!=='-0.37') throw Error('Old snapshot overwrote pending gesture');
    await draining;
    if(!supersededSeen) throw Error('No acknowledgement for coalesced backend requests');
    onFrame({lua:oldMessage}); await new Promise(requestAnimationFrame);
    if(el('volume').value!=='4' || el('pan').value!=='-0.37') throw Error('Old snapshot overwrote confirmed final value');
    if (!framesSeen || messageStates) throw Error('Incorrect state channel');
    await reaper.SetExtState('EEL.Mixer.Test','sliderChecks','confirmed',false);
    el('mute').click();el('solo').click();
    el('color').value='#12ab34';el('color').dispatchEvent(new Event('change'));
    await wait(async () => await reaper.GetMediaTrackInfo_Value(track,'D_VOL')===4 && await reaper.GetMediaTrackInfo_Value(track,'I_SOLO')===2 && el('color').value==='#12ab34' && el('pan').value==='-0.37');
    await mark('edited');
    phase='external';
    await wait(() => el('volume').value==='3.5' && el('pan').value==='0.62' && el('color').value==='#aa22cc' && el('mute').getAttribute('aria-pressed')==='false' && el('solo').getAttribute('aria-pressed')==='false');
    await mark('external');
    await wait(() => next('stale'));
    let rejected=false;
    try { await request('volume',.12,oldState); } catch(error) { rejected=error.message.includes('stale'); }
    if(!rejected) throw Error('Stale selection request was not rejected');
    await new Promise(resolve => setTimeout(resolve,300));
    await mark('stale');
    await wait(() => next('reset'));
    await wait(() => el('name').textContent.startsWith('鼓组'));
    el('reset-color').click();
    await wait(() => el('color-state').textContent==='Default track color');
    await mark('reset');
    await wait(() => next('reload'));
    sessionStorage.reloading='yes'; await reaper.window.reload();
  } catch(error) { await mark('FAIL: '+phase+' '+error.message+' '+lastMessage+' UI '+el('name').textContent+' '+el('volume').value+' '+el('pan').value+' '+el('color').value); }
})();
