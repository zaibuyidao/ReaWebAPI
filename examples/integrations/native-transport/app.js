'use strict';
const el = id => document.getElementById(id);
const messages = {
  en: {
    language:'Language', title:'Realtime Transport Monitor', subtitle:'Lua opens the window and exits · No defer loop · No position interpolation',
    position:'Play Position / seconds', transport:'Transport State', mode:'Subscription mode', frame:'Frame comparison',
    play:'Play', pause:'Pause / Resume', stop:'Stop', reset:'Reset statistics', export:'Export CSV', diagnostics:'Read diagnostics',
    arrivalRate:'Receive rate · last 5 seconds', arrivalInterval:'Mean / P95 interval · last 5 seconds', nativeRate:'Native sample rate · last 5 seconds',
    nativeInterval:'Native mean / P95 interval', maxInterval:'Maximum interval · this run', counts:'Received packets / sequence gaps', positionRate:'Position changes · last 5 seconds',
    chart:'Packet arrival intervals · last 180 packets, milliseconds', raw:'Latest transport snapshot', native:'Native diagnostics',
    timingNote:'Realtime samples on an independent native thread. Scheduling and browser load can delay delivery. Frame is the existing channel used for comparison. Intervals are not end-to-end latency. Host and page clocks cannot be subtracted directly.',
    diagnosticsHint:'Click “Read diagnostics”. Lua writes should be 0.', csvNote:'CSV contains the latest 20,000 transport samples. All readings come from actual callbacks. Playback controls affect the current project.',
    connecting:'Connecting to REAPER…', waitingFirst:'Waiting for the first packet…', unavailable:'Unavailable', paused:'Paused', recordingPaused:'Recording paused', recording:'Recording', playing:'Playing', stopped:'Stopped',
    bits:'Raw {state} · play {play} / pause {pause} / record {record}', noTimestamp:'No sample timestamp',
    waiting:'Waiting for data · no update for {seconds} seconds', receiving:'{mode} · {count} packets received · No Lua loop',
    switching:'Subscription switch in progress', saveTitle:'Save transport CSV', launchError:'Run the Lua launcher in REAPER.',
    versionError:'Requires ReaWebAPI v0.3.9.23 or later.', unsupported:'Realtime transport is unavailable on this host.'
  },
  'zh-CN': {
    language:'语言', title:'实时播放状态', subtitle:'Lua 仅打开窗口后退出 · 无 defer 循环 · 无位置插值',
    position:'Play Position / 秒', transport:'走带状态', mode:'订阅方式', frame:'Frame 对照',
    play:'播放', pause:'暂停 / 继续', stop:'停止', reset:'重置统计', export:'导出 CSV', diagnostics:'读取原生诊断',
    arrivalRate:'实际接收频率 · 最近 5 秒', arrivalInterval:'平均 / P95 间隔 · 最近 5 秒', nativeRate:'原生采样频率 · 最近 5 秒',
    nativeInterval:'原生平均 / P95 间隔', maxInterval:'最大间隔 · 本次统计', counts:'收到包数 / 序号缺口', positionRate:'播放位置实际变化频率 · 最近 5 秒',
    chart:'包到达间隔 · 最近 180 次，毫秒', raw:'最新播放状态快照', native:'原生诊断',
    timingNote:'Realtime 使用独立原生线程采样，系统调度与浏览器负载仍可能延迟交付。Frame 为原有通道对照。采样与接收间隔不代表端到端延迟，宿主时间戳不能直接与网页时钟相减。',
    diagnosticsHint:'点击「读取原生诊断」。Lua 写入次数应为 0。', csvNote:'CSV 保存最近 20,000 个播放状态样本。所有读数来自真实回调。播放按钮会控制当前工程。',
    connecting:'正在连接 REAPER…', waitingFirst:'等待第一个数据包…', unavailable:'不可用', paused:'已暂停', recordingPaused:'录音暂停', recording:'录音中', playing:'播放中', stopped:'已停止',
    bits:'原始值 {state} · play {play} / pause {pause} / record {record}', noTimestamp:'无采样时间戳',
    waiting:'等待数据 · {seconds} 秒未更新', receiving:'{mode} · 已接收 {count} 个包 · Lua 无循环',
    switching:'订阅切换中', saveTitle:'保存播放状态 CSV', launchError:'请在 REAPER 中运行 Lua 启动脚本。',
    versionError:'需要 ReaWebAPI v0.3.9.23 或更高版本。', unsupported:'当前宿主不支持 Realtime transport。'
  }
};
let language='en', ready=false, lastError=null, diagnosticSnapshot=null;
const t=(key, values={})=>messages[language][key].replace(/\{(\w+)\}/g,(_,name)=>String(values[name]??''));
const uiError=key=>Object.assign(new Error(t(key)),{messageKey:key});
function setLanguage(value) {
  language=value==='zh-CN'?'zh-CN':'en';document.documentElement.lang=language;document.title=t('title');
  el('language').value=language;
  document.querySelectorAll('[data-i18n]').forEach(node=>{node.textContent=t(node.dataset.i18n);});
  el('mode').setAttribute('aria-label',t('mode'));
  if(!diagnosticSnapshot)el('native').textContent=t('diagnosticsHint');
  if(latest)renderTransport();
  draw();
}
function renderTransport() {
  const state=mode==='frame'?latest.playing:latest.playState;
  el('position').textContent=fmt(latest.position,6);el('transport').textContent=stateName(state);
  el('bits').textContent=t('bits',{state,play:state&1?1:0,pause:state&2?1:0,record:state&4?1:0});
}
const show = value => JSON.stringify(value, (_, v) => typeof v === 'bigint' ? v.toString() : v, 2);
const finite = value => typeof value === 'number' && Number.isFinite(value);
const fmt = (value, n = 2) => finite(value) ? value.toFixed(n) : '—';
let samples = [], latest = null, dispose = null, previous = null, mode = '60', failed = false;
let total = 0, gaps = 0, maxGap = 0, started = performance.now(), switching = false;
function stateName(s) { return t(!Number.isInteger(s)?'unavailable':s&2?(s&4?'recordingPaused':'paused'):s&4?'recording':s&1?'playing':'stopped'); }
function reset() { samples = []; total = 0; gaps = 0; maxGap = 0; previous = null; latest = null; started = performance.now(); }
function receive(value) {
  const now = performance.now(), state = mode === 'frame' ? value.playing : value.playState;
  const sequence = Number(value.sequence), sampledAt = value.sampledAt ?? null;
  const dt = previous ? now - previous.receivedMs : null;
  const missing = previous ? Math.max(0, sequence - previous.sequence - 1) : 0;
  const row = { receivedMs: now, intervalMs: dt, sampledAt, sampleIntervalMs: previous && sampledAt !== null ? (sampledAt - previous.sampledAt) * 1000 : null,
    sequence, position: value.position, state, playRate:value.playRate, tempo:value.tempo, beatPosition:value.beatPosition,
    numerator:value.timeSignature?.numerator, denominator:value.timeSignature?.denominator, missing, projectEpoch: value.projectEpoch?.toString() ?? '' };
  samples.push(row); if (samples.length > 20000) samples.shift();
  previous = row; latest = value; total++; gaps += missing; maxGap = Math.max(maxGap, dt ?? 0);
  renderTransport();
}
function metrics(rows, key) {
  if (rows.length < 2) return {hz:0, mean:null, p95:null, max:null};
  const intervals = rows.slice(1).map((r,i) => r[key] - rows[i][key]).sort((a,b)=>a-b);
  const span = rows.at(-1)[key] - rows[0][key];
  return {hz: span > 0 ? (rows.length-1)*1000/span : 0, mean:span/(rows.length-1),
    p95: intervals[Math.ceil(intervals.length*.95)-1], max:intervals.at(-1)};
}
function csv(rows) {
  return 'received_page_ms,arrival_interval_ms,sampled_host_seconds,sample_interval_ms,sequence,position_seconds,play_state,sequence_gap,project_epoch,play_rate,tempo_bpm,qn_position,time_signature_numerator,time_signature_denominator\n' +
    rows.map(r=>[r.receivedMs,r.intervalMs??'',r.sampledAt??'',r.sampleIntervalMs??'',r.sequence,r.position,r.state,r.missing,r.projectEpoch,r.playRate??'',r.tempo??'',r.beatPosition??'',r.numerator??'',r.denominator??''].join(',')).join('\n');
}
function report(error) { failed=true;lastError=error;draw(); }
async function switchMode(next) {
  if (switching) throw uiError('switching');
  switching = true; el('mode').disabled = true;
  try {
    if (dispose) { const close = dispose; dispose = null; await close(); }
    mode = next; el('mode').value = next; reset(); failed = false;
    if (mode === 'frame') dispose = await reaper.events.on('frame', receive);
    else {
      const sub = await reaper.realtime.subscribe('transport', { updateRate:Number(mode), onData:receive, onError:report });
      dispose = () => sub.close();
    }
  } finally { switching = false; el('mode').disabled = false; }
}
function draw() {
  const now = performance.now(), rows = samples.filter(r => r.receivedMs >= now-5000);
  const arrival = metrics(rows,'receivedMs');
  const native = metrics(rows.filter(r=>r.sampledAt!==null).map(r=>({...r,t:r.sampledAt*1000})),'t');
  const age = previous ? now-previous.receivedMs : now-started;
  el('hz').textContent = `${fmt(age>1500?0:arrival.hz,1)} Hz`;
  el('native-hz').textContent = mode === 'frame' ? t('noTimestamp') : `${fmt(age>1500?0:native.hz,1)} Hz`;
  el('interval').textContent = `${fmt(arrival.mean)} / ${fmt(arrival.p95)} ms`;
  el('native-interval').textContent = `${fmt(native.mean)} / ${fmt(native.p95)} ms`;
  const span = rows.length>1 ? (rows.at(-1).receivedMs-rows[0].receivedMs)/1000 : 0;
  const changes = rows.slice(1).filter((r,i)=>r.position!==rows[i].position).length;
  el('position-hz').textContent = `${fmt(age>1500||!span?0:changes/span,1)} Hz`;
  el('max').textContent = `${fmt(total>1?maxGap:null)} ms`; el('counts').textContent = `${total} / ${gaps}`;
  el('raw').textContent=latest?show(latest):t('waitingFirst');
  el('status').textContent=failed?`${lastError.code||'ERROR'}: ${lastError.messageKey?t(lastError.messageKey):lastError.message}`:
    !ready?t('connecting'):age>1500?t('waiting',{seconds:fmt(age/1000,1)}):t('receiving',{mode:mode==='frame'?t('frame'):'Realtime '+mode+' Hz',count:total});
  const canvas = el('chart'), ctx = canvas.getContext('2d');
  const values = samples.slice(-180).map(r=>r.intervalMs).filter(finite), ceiling = Math.max(25,...values)*1.15;
  ctx.clearRect(0,0,canvas.width,canvas.height);ctx.fillStyle='#9caec4';ctx.font='12px Consolas';ctx.fillText(`${ceiling.toFixed(0)} ms`,8,16);
  ctx.strokeStyle='#63ddbb';ctx.beginPath();values.forEach((v,i)=>{const x=60+i/179*930,y=120-v/ceiling*100;if(i)ctx.lineTo(x,y);else ctx.moveTo(x,y);});ctx.stroke();
}
const action = fn => async () => {try {await fn();} catch(error) {report(error);}};
el('mode').onchange = action(()=>switchMode(el('mode').value));
el('reset').onclick = reset;
el('play').onclick = action(()=>reaper.OnPlayButton());
el('pause').onclick = action(()=>reaper.OnPauseButton());
el('stop').onclick = action(()=>reaper.OnStopButton());
async function readDiagnostics() {
  diagnosticSnapshot={realtime:await reaper.realtime.getDiagnostics(),frames:(await reaper.debug.getDiagnostics()).frames};
  el('native').textContent=show(diagnosticSnapshot);
}
el('diagnostics').onclick = action(readDiagnostics);
el('export').onclick = action(async()=>{const rows=samples.slice();const path=await reaper.dialog.saveFile({title:t('saveTitle')});if(path)await reaper.fs.writeFile(path,csv(rows));});
window.addEventListener('pagehide',()=>{if(dispose)void dispose().catch(()=>{});});
el('language').onchange=()=>setLanguage(el('language').value);
setLanguage('en');
window.demoReady = (async()=>{
  if(!window.reaper)throw uiError('launchError');
  await reaper.lifecycle.ready;
  if(!reaper.realtime)throw uiError('versionError');
  const capabilities=await reaper.realtime.getCapabilities();
  if(!capabilities.sources.transport?.available)throw uiError('unsupported');
  await switchMode('60');
  for(const id of ['play','pause','stop','export','diagnostics'])el(id).disabled=false;
  ready=true;draw();setInterval(draw,100);
})();
window.demoReady.catch(report);
