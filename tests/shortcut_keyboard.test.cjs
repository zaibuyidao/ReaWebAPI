const {test}=require('node:test');
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const script=fs.readFileSync(path.join(__dirname,'../src/platform/shared/shortcut_keyboard.hpp'),'utf8').match(/R"JS\(([\s\S]*?)\)JS"/)[1];
function fixture(enabled=true) {
  const events={},sent=[];
  class Element {attachShadow(){return {activeElement:null};}}
  const document={activeElement:null,designMode:'off'};
  const window={chrome:{webview:{postMessage:value=>sent.push(JSON.parse(value.slice(17)))}}};window.top=window;
  vm.runInNewContext(script,{window,document,Element,addEventListener:(type,fn)=>{events[type]=fn;}});
  const enable=(value,doc='first')=>window.__reawebSetShortcutRouting(value,doc);enable(enabled);
  const key=(type,extra={})=>{
    const e={code:'KeyA',key:'a',isTrusted:true,repeat:false,composedPath:()=>[],preventDefault(){this.prevented=true;},stopImmediatePropagation(){this.stopped=true;},...extra};
    events[type](e);return e;
  };
  return {events,sent,document,Element,enable,key,release:window.__reawebReleaseShortcut};
}
test('routes letters, navigation, function keys, punctuation and modifier chords',()=>{
  for(const [code,key,value] of [['KeyA','a',65],['ArrowLeft','ArrowLeft',37],['F8','F8',119],['Semicolon',':',186],['Digit2','@',50],['Space',' ',32],['Numpad1','1',97]]) {
    const f=fixture();
    assert.ok(f.key('keydown',{code,key,ctrlKey:true,shiftKey:true}).prevented);
    assert.ok(f.key('keyup',{code,key}).prevented);
    assert.deepEqual(f.sent.map(e=>[e.key,e.modifiers,e.down,e.document]),[[value,12,true,'first'],[value,12,false,'first']]);
  }
});
test('text, closed shadow roots, designMode and IME retain entire presses',()=>{
  for(const target of [{tagName:'INPUT',type:'text'},{tagName:'TEXTAREA'},{isContentEditable:true},{tagName:'IFRAME'}]) {
    const f=fixture();f.document.activeElement=target;
    assert.ok(!f.key('keydown').prevented);f.document.activeElement=null;
    assert.ok(!f.key('keydown',{repeat:true}).prevented);assert.ok(!f.key('keyup').prevented);assert.equal(f.sent.length,0);
  }
  const f=fixture(),host=new f.Element();host.attachShadow({mode:'closed'}).activeElement={tagName:'INPUT',type:'text'};
  f.document.activeElement=host;f.key('keydown');f.key('keyup');f.document.activeElement=null;
  f.document.designMode='on';f.key('keydown');f.key('keyup');f.document.designMode='off';
  f.events.compositionstart();f.key('keydown');f.key('keyup');f.events.compositionend();
  for(const extra of [{isComposing:true},{keyCode:229},{isTrusted:false}]){f.key('keydown',extra);f.key('keyup',extra);}
  assert.equal(f.sent.length,0);
});
test('disable, blur and reload release once and suppress held tails',()=>{
  for(const finish of [f=>f.enable(false),f=>f.events.blur(),f=>f.enable(true,'second'),f=>f.events.pagehide()]) {
    const f=fixture();f.key('keydown');f.key('keydown',{repeat:true,altKey:true});finish(f);
    assert.ok(f.key('keydown',{repeat:true}).prevented);assert.ok(f.key('keyup').prevented);
    assert.deepEqual(f.sent.map(e=>[e.down,e.repeat,e.modifiers,e.document]),[[true,false,0,'first'],[true,true,0,'first'],[false,false,0,'first']]);
  }
});
test('default off, enabling a held page key and modifier-only presses do not route',()=>{
  const f=fixture(false);f.key('keydown');f.enable(true);f.key('keydown',{repeat:true});f.key('keyup');
  f.key('keydown',{code:'ShiftLeft',key:'Shift'});f.key('keyup',{code:'ShiftLeft',key:'Shift'});
  assert.equal(f.sent.length,0);f.key('keydown');f.key('keyup');assert.equal(f.sent.length,2);
});
test('native Command release suppresses a trailing DOM release and permits a changed chord',()=>{
  const f=fixture();f.key('keydown',{metaKey:true});f.release(65,0);f.release(65,0);
  assert.ok(f.key('keyup').prevented);
  f.key('keydown',{metaKey:true,shiftKey:true});f.key('keyup');
  assert.deepEqual(f.sent.map(e=>[e.down,e.modifiers]),[[true,32],[false,32],[true,36],[false,36]]);
  f.key('keydown');f.key('keydown',{shiftKey:true});f.key('keyup');
  assert.deepEqual(f.sent.slice(-4).map(e=>[e.down,e.modifiers]),[[true,0],[false,0],[true,4],[false,4]]);
});
