"""Opt-in real REAPER/WebView2 benchmark. Never drives the user's REAPER instance.

Creates a disposable portable copy under latency-results; uses unchanged installed
DLL and DSP. Observes existing diagnostics: reveal is NOT compositor visibility.
Run with --prepare, then --run NAME --suite smoke|warm|cold-app|cold-jsfx.
"""
from pathlib import Path
import argparse, json, shutil, subprocess, time, hashlib, os
import psutil

ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'latency-results'/'2026-10-22'
PORT=BASE/'portable'
SOURCE=Path(r'C:\REAPER')
FX=SOURCE/'Effects'/'zaibuyidao Scripts'/'JSFX'/'ReaWebAPI'/'1175 Compressor'

PROBE=r'''(() => {
const marks={script:performance.now(),viewport:{width:innerWidth,height:innerHeight,dpr:devicePixelRatio}};window.__latencyMarks=marks;
document.addEventListener('DOMContentLoaded',()=>marks.dom=performance.now(),{once:true});
window.addEventListener('load',()=>marks.load=performance.now(),{once:true});
reaper.lifecycle.ready.then(()=>marks.sdk=performance.now());
window.__latencyDone=async()=>{
 marks.appReady=performance.now();
 await new Promise(requestAnimationFrame);marks.raf1=performance.now();
 await new Promise(requestAnimationFrame);marks.raf2=performance.now();
 const button=document.querySelector('#probe')||document.querySelector('.styles button[data-theme="ae"]')||document.querySelector('#refresh')||document.querySelector('#read-project');
 if(button&&!button.disabled){button.click();marks.syntheticClick=performance.now();marks.syntheticAck=button.id==='probe'?document.querySelector('#ack').textContent==='clicked':button.dataset.theme?document.body.dataset.theme==='ae':null;}
 marks.navigation=performance.getEntriesByType('navigation')[0]?.toJSON();
 await reaper.debug.log('LATENCY_PAGE '+JSON.stringify(marks));
};
})();'''

MINIMAL='''<!doctype html><meta charset="utf-8"><title>Latency minimal</title>
<style>body{background:#242424;color:white;font:16px sans-serif}button{padding:12px}</style>
<h1>Latency baseline</h1><button id="probe">Click</button><span id="ack"></span>
<script src="probe.js"></script><script type="module">
import {connectJSFX} from './jsfx-webui.mjs';
await reaper.lifecycle.ready;
probe.onclick=()=>ack.textContent='clicked';
if(document.documentElement.dataset.hosted==='yes'){
 const jsfx=await connectJSFX(reaper,e=>reaper.debug.error(String(e)));
 await jsfx.onVisibilityChange(()=>{});
}
await window.__latencyDone();
</script>'''

def prepare():
    PORT.mkdir(parents=True,exist_ok=True)
    for directory in ['Plugins','UserPlugins','Scripts','Effects/Latency']:(PORT/directory).mkdir(parents=True,exist_ok=True)
    for name in ['reaper.exe','reaper-license.rk']:shutil.copy2(SOURCE/name,PORT/name)
    for name in ['jsfx.dll','reaper_midi.dll','reaper_wave.dll','reaper_csurf.dll']:shutil.copy2(SOURCE/'Plugins'/name,PORT/'Plugins'/name)
    shutil.copy2(SOURCE/'UserPlugins/reaper_reawebapi-x64.dll',PORT/'UserPlugins')
    shutil.copy2(ROOT/'tests/startup_latency_windows.lua',PORT/'Scripts/runner.lua')
    for page in ['1175','minimal']:
        dest=PORT/'Effects/Latency'/page
        shutil.copytree(FX,dest,dirs_exist_ok=True)
        web=dest/'.webui'
        (web/'probe.js').write_text(PROBE,encoding='utf-8')
        if page=='minimal':
            (web/'index.html').write_text(MINIMAL.replace('<meta','<html data-hosted="yes"><meta',1),encoding='utf-8')
        else:
            html=(web/'index.html').read_text(encoding='utf-8').replace('<head>','<head><script src="probe.js"></script>')
            (web/'index.html').write_text(html,encoding='utf-8')
            with (web/'app.js').open('a',encoding='utf-8') as f:f.write('\nif(jsfx) await window.__latencyDone();\n')
    dest=PORT/'Scripts/minimal';dest.mkdir(exist_ok=True)
    (dest/'app.json').write_text('{"id":"latency-minimal","name":"Latency minimal","version":"1.0.0"}',encoding='utf-8')
    (dest/'index.html').write_text(MINIMAL,encoding='utf-8');(dest/'probe.js').write_text(PROBE,encoding='utf-8')
    shutil.copy2(FX/'.webui/jsfx-webui.mjs',dest)
    dest=PORT/'Scripts/starter';shutil.copytree(ROOT/'examples/templates/starter',dest,dirs_exist_ok=True)
    (dest/'probe.js').write_text(PROBE,encoding='utf-8')
    p=dest/'index.html';p.write_text(p.read_text(encoding='utf-8').replace('<head>','<head><script src="probe.js"></script>'),encoding='utf-8')
    with (dest/'app.js').open('a',encoding='utf-8') as f:
        f.write("\n{const b=document.getElementById('refresh');const ob=new MutationObserver(()=>{if(!b.disabled){ob.disconnect();window.__latencyDone();}});ob.observe(b,{attributes:true,attributeFilter:['disabled']});}\n")
    dest=PORT/'Scripts/demo';shutil.copytree(ROOT/'examples/main-demo',dest,dirs_exist_ok=True)
    (dest/'probe.js').write_text(PROBE,encoding='utf-8')
    p=dest/'index.html';p.write_text(p.read_text(encoding='utf-8').replace('<head>','<head><script src="probe.js"></script>'),encoding='utf-8')
    with (dest/'app.js').open('a',encoding='utf-8') as f:
        f.write("\n{const b=document.getElementById('read-project');const ob=new MutationObserver(()=>{if(!b.disabled){ob.disconnect();window.__latencyDone();}});ob.observe(b,{attributes:true,attributeFilter:['disabled']});}\n")
    hashes={str(p.relative_to(FX)):hashlib.sha256(p.read_bytes()).hexdigest() for p in FX.rglob('*') if p.is_file()}
    (BASE/'input-hashes.json').write_text(json.dumps({'fx':hashes,'dll':hashlib.sha256((PORT/'UserPlugins/reaper_reawebapi-x64.dll').read_bytes()).hexdigest()},indent=2),encoding='utf-8')

def lua(v):
    if isinstance(v,str):return json.dumps(v,ensure_ascii=False)
    if isinstance(v,bool):return 'true' if v else 'false'
    if isinstance(v,(float,int)):return str(v)
    if isinstance(v,list):return '{'+','.join(map(lua,v))+'}'
    return '{'+','.join('['+lua(k)+']='+lua(x) for k,x in v.items())+'}'

def cases(suite,page):
    if suite=='smoke':return [dict(scenario='A2',page='minimal',samples=1),dict(scenario='J2',page='1175',samples=1)]
    if suite.startswith('cold-'):return [dict(scenario='A1' if suite=='cold-app' else 'J1',page=page,cold=True,samples=1)]
    if suite=='warm':return [dict(scenario=s,page=p) for p in ['minimal','starter'] for s in ['A2','A3','A4']]+[dict(scenario=s,page=p,mode=m) for p in ['minimal','1175'] for s,m in [('J2','floating'),('J3','floating'),('J4','floating'),('J5','chain'),('J6-new','chain')]]
    if suite=='scale':return [dict(scenario='J2',page=p,instances=n) for p in ['minimal','1175'] for n in [10,30]]
    if suite=='take':return [dict(scenario=s,page=p,owner='take') for p in ['minimal','1175'] for s in ['J2','J3']]
    if suite=='container':return [dict(scenario=s,page=p,owner='container') for p in ['minimal','1175'] for s in ['J2','J3']]
    if suite=='switch':return [dict(scenario='J5-switch',page=p,mode='chain',instances=3) for p in ['minimal','1175']]
    if suite=='host':return [dict(scenario='J3',page=p) for p in ['minimal','1175']]
    if suite=='demo':return [dict(scenario='A2',page='demo')]
    if suite=='resident':return [dict(scenario=s,page=p,resident=True) for s,p in [('A2','minimal'),('J2','minimal'),('J2','1175')]]
    if suite=='profile':return [dict(scenario='P-canvas',page='minimal')]
    if suite=='background':return [dict(scenario='P-background',page='minimal')]
    if suite=='prewarm':return [dict(scenario=s,page=p) for p in ['minimal','1175'] for s in (['A2'] if p=='minimal' else ['J2'])]
    raise ValueError(suite)

def run(name,suite,page,idle,settle=0):
    if Path(name).name!=name:raise ValueError('Run name must not contain a directory')
    output=BASE/(name+'.jsonl')
    if output.exists():raise FileExistsError(f'Use a new run name to preserve existing data: {output}')
    exe=PORT/'reaper.exe'
    for proc in psutil.process_iter(['pid','exe']):
        if proc.info['exe'] and Path(proc.info['exe'])==exe:
            descendants=proc.children(recursive=True)
            proc.terminate();proc.wait(10)
            _,alive=psutil.wait_procs(descendants,timeout=8)
            if alive:raise RuntimeError('Previous test browser processes have not exited')
    (PORT/'ReaWebAPI.ini').write_text(f'[ReaWebAPI]\nIdleWebViews={idle}\nWebViewColorProfile=sRGB\n')
    config=dict(run=name,idle=idle,output=output.as_posix(),cases=cases(suite,page),resident=suite=='resident',settle=settle)
    for case in config['cases']:
        if case['scenario']=='J5':case['instances']=3
    (PORT/'Scripts/latency-config.lua').write_text('return '+lua(config),encoding='utf-8')
    (BASE/(name+'-config.json')).write_text(json.dumps(config,indent=2))
    status=Path(str(output)+'.status')
    if status.exists():status.unlink()
    before=time.perf_counter()
    p=subprocess.Popen([str(exe),'-newinst','-nosplash',str(PORT/'Scripts/runner.lua')],cwd=PORT)
    records=[];previous={};last='';last_print=0
    while p.poll() is None and time.perf_counter()-before<1200:
        now=time.perf_counter();state=status.read_text(encoding='utf-8') if status.exists() else 'launching'
        if state!=last and now-last_print>10:print(state,flush=True);last=state;last_print=now
        try:
            parent=psutil.Process(p.pid);procs=[parent]+parent.children(recursive=True)
            entries=[]
            for proc in procs:
                try:
                    mem=proc.memory_info();cpu=sum(proc.cpu_times()[:2]);old=previous.get(proc.pid)
                    entries.append(dict(pid=proc.pid,name=proc.name(),private=mem.private,rss=mem.rss,
                      cpuPercentOneCore=(100*(cpu-old[1])/(now-old[0])) if old else None,
                      role=next((x for x in proc.cmdline() if x.startswith('--type=')),'browser' if proc.pid!=p.pid else 'reaper')))
                    previous[proc.pid]=(now,cpu)
                except (psutil.NoSuchProcess,psutil.AccessDenied):pass
            records.append(dict(elapsed=now-before,status=state,processes=entries))
        except psutil.NoSuchProcess:break
        if state=='complete' or state.startswith('ERROR'):break
        time.sleep(.1)
    (BASE/(name+'-resources.json')).write_text(json.dumps(records),encoding='utf-8')
    print('RESULT',name,status.read_text() if status.exists() else 'NO STATUS','pid',p.pid,flush=True)
    # Leave this owned instance alive for visual inspection; next run terminates it.

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--prepare',action='store_true');ap.add_argument('--run');ap.add_argument('--suite',default='smoke');ap.add_argument('--page',default='minimal');ap.add_argument('--idle',type=int,default=0);ap.add_argument('--repeat',type=int,default=1);ap.add_argument('--settle',type=float,default=0)
    args=ap.parse_args()
    if args.prepare:prepare()
    if args.run:
        for n in range(args.repeat):run(args.run+(f'-{n+1:02}' if args.repeat>1 else ''),args.suite,args.page,args.idle,args.settle)
