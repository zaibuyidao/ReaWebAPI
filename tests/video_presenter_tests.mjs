import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
const root = new URL('../examples/integrations/video-preview/', import.meta.url);
const moduleURL = text => `data:text/javascript;base64,${Buffer.from(text).toString('base64')}`;
let source = await readFile(new URL('video-renderer.js', root), 'utf8');
for (const file of ['frame-renderer.js', 'canvas-renderer.js', 'i420-renderer.js']) source = source.replace(`'./${file}'`, JSON.stringify(moduleURL(await readFile(new URL(file, root), 'utf8'))));
source = source.replace("new URL('./video-worker.js', import.meta.url)", "'worker.js'");
const { createVideoRenderer } = await import(moduleURL(source));
let now=0,next=0;const callbacks=new Map();
globalThis.performance={now:()=>now};globalThis.requestAnimationFrame=fn=>{callbacks.set(++next,fn);return next};globalThis.cancelAnimationFrame=id=>callbacks.delete(id);
const flush=async()=>{for(let i=0;i<8;i++)await Promise.resolve()};
const wake=()=>{now+=17;const list=[...callbacks.values()];callbacks.clear();for(const fn of list)fn(now)};
globalThis.ImageData=class{constructor(data,width,height){Object.assign(this,{data,width,height})}};
const frame=n=>({sequence:BigInt(n),timestamp:n/60,bytes:new Uint8Array(16).fill(n)});
{
 let instance;globalThis.Worker=class{
  constructor(){instance=this;this.sent=[];this.terminated=false}
  postMessage(data,transfer){this.sent.push(data.type==='frame'?structuredClone(data,{transfer}):data);if(data.type==='probe'||data.type==='init')queueMicrotask(()=>this.onmessage({data:{type:data.type==='probe'?'probe':'ready',supported:true,kind:'webgl'}}))}
  terminate(){this.terminated=true}
 };
 const drawn=[],r=await createVideoRenderer({transferControlToOffscreen:()=>({})},{onDraw:id=>drawn.push(id)});
 const a=frame(1),b=frame(2),c=frame(3);r.push(a);r.push(b);for(let i=3;i<=6;i++)r.push(frame(i));r.push(c);
 assert.equal(a.bytes.byteLength,0);assert.equal(b.bytes.byteLength,0);assert.equal(c.bytes.byteLength,16);assert.equal(r.dropped,1);
 assert.deepEqual([...instance.sent.find(x=>x.type==='frame').frame.bytes],Array(16).fill(1));
 r.clear();instance.onmessage({data:{type:'draw',generation:0,sequence:1n,dropped:0}});assert.equal(drawn.length,0);
 instance.onmessage({data:{type:'draw',generation:1,sequence:3n,dropped:0}});assert.deepEqual(drawn,[3n]);
 r.resize(3840,2160);assert.deepEqual(instance.sent.at(-1),{type:'resize',width:3840,height:2160,info:undefined});
 r.dispose();assert.ok(instance.terminated);r.push(frame(4));assert.equal(instance.sent.at(-1).type,'resize');
}
{
 const requests=[],shown=[],drawn=[];globalThis.createImageBitmap=(pixels,options)=>new Promise(resolve=>requests.push({pixels,options,resolve}));
 const canvas={width:2,height:2,getContext:type=>type==='bitmaprenderer'?{transferFromImageBitmap:image=>shown.push(image)}:null};
 const r=await createVideoRenderer(canvas,{platform:'macos',onDraw:id=>drawn.push(id)});
 r.push(frame(1));r.push(frame(2));wake();wake();assert.equal(requests.length,2);
 assert.deepEqual(requests[0].options,{premultiplyAlpha:'none',colorSpaceConversion:'none'});
 let closed=0;requests[1].resolve({id:2,close(){closed++}});await flush();requests[0].resolve({id:1,close(){closed++}});await flush();
 assert.deepEqual(drawn,[2n]);assert.equal(closed,1);assert.equal(r.dropped,1);
 r.push(frame(3));wake();r.clear();requests[2].resolve({id:3,close(){closed++}});await flush();
 assert.equal(closed,2);assert.equal(shown.at(-1),null);assert.deepEqual(drawn,[2n]);
 r.resize(3840,2160);assert.equal(canvas.width,3840);assert.equal(canvas.height,2160);r.dispose();assert.equal(callbacks.size,0);
}
console.log('Video worker ownership, backpressure, stale completion and bitmap ordering passed');
{
 const calls = [], events = {}, errors = [], drawn = [];
 const gl = new Proxy({ MAX_TEXTURE_SIZE: 1, COMPILE_STATUS: 2, LINK_STATUS: 3, NO_ERROR: 0,
   getParameter: () => 64, getShaderParameter: () => true, getProgramParameter: () => true, getError: () => 0,
   getAttribLocation: () => 0, getUniformLocation: () => ({}),
 }, { get(target, key) { return target[key] ?? (String(key).startsWith('create') ? () => ({}) : (...args) => calls.push([key, ...args])); } });
 const canvas = { width: 2, height: 2, getContext: name => name === 'webgl2' ? gl : null,
   addEventListener: (name, fn) => { events[name] = fn; }, removeEventListener: name => { delete events[name]; } };
 const r = await createVideoRenderer(canvas, { platform: 'macos', format: 'i420', onDraw: id => drawn.push(id), onError: error => errors.push(error.code) });
 const info = { format: 'i420', colorMatrix: 'bt709', colorRange: 'limited', colorConversion: 'reaper-neon' };
 r.resize(6, 2, info); r.push({ sequence: 1n, timestamp: 0, bytes: new Uint8Array(18) }); wake();
 assert.deepEqual(drawn, [1n]);
 assert.deepEqual(calls.filter(c => c[0] === 'texImage2D').at(-1).slice(4, 6), [6, 3]);
 assert.equal(calls.filter(c => c[0] === 'texSubImage2D').length, 1, 'All planes use one texture upload');
 const uploads = calls.filter(c => c[0] === 'texSubImage2D').length;
 r.present(3, 6, true);
 assert.deepEqual([canvas.width, canvas.height], [3, 6]);
 assert.deepEqual(calls.filter(c => c[0] === 'uniform4f').at(-1).slice(2), [0, 2.5, 3, 1], 'Contain preserves the source aspect inside the display surface');
 assert.equal(calls.filter(c => c[0] === 'texSubImage2D').length, uploads, 'Resizing redraws the original texture without another pixel upload');
 r.present(3, 6, false);
 assert.deepEqual(calls.filter(c => c[0] === 'uniform4f').at(-1).slice(2), [0, 0, 3, 6], 'Fill uses the entire display surface');
 assert.deepEqual(drawn, [1n], 'Presentation changes do not count as new video frames');
 r.clear(); const draws = calls.filter(c => c[0] === 'drawArrays').length;
 r.present(6, 2, true);
 assert.equal(calls.filter(c => c[0] === 'drawArrays').length, draws, 'A cleared frame is not restored by a display resize');
 assert.throws(() => r.present(65, 2, true), { code: 'VIDEO_RENDER_LIMIT' });
 assert.throws(() => r.resize(6, 2, { ...info, colorMatrix: 'bt601' }), { code: 'VIDEO_RENDER_UNSUPPORTED' });
 assert.throws(() => r.resize(6, 2, { ...info, colorConversion: 'unknown' }), { code: 'VIDEO_RENDER_UNSUPPORTED' });
 events.webglcontextlost({ preventDefault() {} }); r.push({ sequence: 2n, timestamp: 0.1, bytes: new Uint8Array(18) }); wake(); assert.equal(drawn.length, 1);
 events.webglcontextrestored(); r.resize(6, 2, info); r.push({ sequence: 3n, timestamp: 0.2, bytes: new Uint8Array(18) }); wake(); assert.deepEqual(drawn, [1n, 3n]);
 assert.throws(() => r.resize(64, 48, info), { code: 'VIDEO_RENDER_LIMIT' });
 r.resize(6, 2, info); r.push({ sequence: 4n, timestamp: 0.3, bytes: new Uint8Array(17) }); wake(); assert.deepEqual(errors, ['VIDEO_RENDER_LIMIT']);
 r.dispose(); assert.equal(Object.keys(events).length, 0);
}
console.log('I420 single-upload layout, color validation, context recovery and texture limits passed');
