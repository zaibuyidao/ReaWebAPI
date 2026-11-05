import {mouseRegions} from './regions.js';
const $=selector=>document.querySelector(selector);
const fail=error=>{$('#status').textContent=`${error.code || 'ERROR'}: ${error.message}`;};
try {
  await reaper.lifecycle.ready;
  await reaper.window.setShortcutRouting(true);
  const platform=await reaper.system.getPlatform();
  const elements=[...document.querySelectorAll('.interactive')];
  let scheduled=false,disposed=false,publishing=false,dirty=false;
  const publish=async()=>{
    dirty=true;
    if (publishing) return;
    publishing=true;
    try { while(dirty && !disposed) {dirty=false;await reaper.window.setMouseRegions(mouseRegions(elements));} }
    catch(error) {fail(error);}
    finally {publishing=false;}
  };
  const update=()=>{
    if(scheduled || disposed) return;
    scheduled=true;requestAnimationFrame(()=>{scheduled=false;publish();});
  };
  const resize=new ResizeObserver(update);
  elements.forEach(element=>resize.observe(element));
  const layout=new MutationObserver(update);
  layout.observe(document.querySelector('main'),{attributes:true,subtree:true,attributeFilter:['class','d','r','cx','cy','transform','viewBox']});
  addEventListener('resize',update);addEventListener('scroll',update,true);
  const fit=async()=>{
    const displays=await reaper.system.getDisplays(),position=await reaper.window.getPosition();
    const display=displays.find(({bounds:b})=>position.x>=b.x && position.x<b.x+b.width && position.y>=b.y && position.y<b.y+b.height) || displays.find(d=>d.primary) || displays[0];
    if(!display) return;
    const {x,y,width,height}=display.bounds;
    // Avoid X11 window managers promoting an exact screen-sized window to a fullscreen layer.
    await reaper.window.setSize(width,height+(platform==='linux'?1:0));await reaper.window.setPosition(x,y);await publish();
  };
  $('#mode').onchange=()=>reaper.window.setMouseMode($('#mode').value).catch(fail);
  $('#shortcuts').onchange=()=>reaper.window.setShortcutRouting($('#shortcuts').checked).catch(fail);
  $('#play').onclick=()=>reaper.Main_OnCommand(40044,0).catch(fail);
  $('#close').onclick=()=>reaper.window.close().catch(fail);
  $('#fit').onclick=()=>fit().catch(fail);
  const master=await reaper.GetMasterTrack(0);
  for(const spec of [{id:'gain',parameter:'D_VOL',min:-60,max:12},{id:'pan',parameter:'D_PAN',min:-100,max:100}]) {
    const control=$('#'+spec.id),initial=await reaper.GetMediaTrackInfo_Value(master,spec.parameter);
    let value=spec.id==='gain'?Math.min(12,Math.max(-60,20*Math.log10(Math.max(initial,0.001)))):initial*100;
    let drag=null,pending=null,writing=false;
    const draw=()=>{
      $('#'+spec.id+'-needle').style.transform=`rotate(${-135+270*(value-spec.min)/(spec.max-spec.min)}deg)`;
      $('#'+spec.id+'-value').textContent=spec.id==='gain'?`${value.toFixed(1)} dB`:`${value.toFixed(0)} %`;
      control.setAttribute('aria-valuenow',value.toFixed(1));
    };
    const write=async()=>{
      pending=spec.id==='gain'?10**(value/20):value/100;
      if(writing) return;
      writing=true;
      try {while(pending!==null && !disposed) {const next=pending;pending=null;await reaper.SetMediaTrackInfo_Value(master,spec.parameter,next);}}
      catch(error) {fail(error);}
      finally {writing=false;}
    };
    control.setAttribute('aria-valuemin',spec.min);control.setAttribute('aria-valuemax',spec.max);draw();
    control.onpointerdown=event=>{
      if(event.button!==0) return;
      event.preventDefault();control.setPointerCapture(event.pointerId);
      drag={id:event.pointerId,y:event.clientY};
    };
    control.onpointermove=event=>{
      if(!drag || drag.id!==event.pointerId) return;
      value=Math.max(spec.min,Math.min(spec.max,value+(drag.y-event.clientY)*(spec.max-spec.min)/(event.shiftKey?1500:150)));
      drag.y=event.clientY;
      draw();write();
    };
    const end=event=>{
      if(!drag || drag.id!==event.pointerId) return;
      drag=null;
      if(control.hasPointerCapture(event.pointerId)) control.releasePointerCapture(event.pointerId);
    };
    control.onpointerup=end;control.onpointercancel=end;control.onlostpointercapture=()=>{drag=null;};
  }
  await reaper.events.on('windowstatechange',state=>{
    $('#mode').value=state.mouseMode;$('#shortcuts').checked=state.shortcutRouting;
    $('#status').textContent=`${state.layer} · ${state.mouseMode} · Main shortcuts ${state.shortcutRouting?'on':'off'}`;
  });
  await reaper.lifecycle.on('cleanup',()=>{disposed=true;resize.disconnect();layout.disconnect();removeEventListener('resize',update);removeEventListener('scroll',update,true);});
  await fit();await publish();
} catch(error) {fail(error);}
