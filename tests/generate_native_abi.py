"""Local exhaustive ABI exercise; native callees are harmless independent C++ stubs."""
import json,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools.native_bindings import plans,canonical,HANDLES
schema=json.loads((ROOT/'api/reaper_api.json').read_text())
rows=plans(schema)
lines=['#include "core/native_call.hpp"','#include <cstring>','#include <iostream>', 'using namespace reaweb;', '#define CHECK(x) do {if(!(x)) throw std::runtime_error(std::string(active)+": " #x);}while(false)', 'static const char* active="setup";', 'static int calls[730]{};', 'static GUID guid{0x12345678,0x1234,0x5678,{1,2,3,4,5,6,7,8}};', 'static int register_buffer(char**,int*) { return 123; }', 'static void clear_buffer(int t) { CHECK(t==123); }']
inputs={};outputs={}
pointers={t: 4096+i*256 for i,t in enumerate(sorted(HANDLES))}
for fi,row in enumerate(rows):
 n=row['name'];fn=schema['functions'][n]
 def value(p,i):
  t=p['type']
  if t=='integer': return 3 if p['name']=='numsamplesperchannel' else 2 if p['name']=='numchannels' else 1 if p['name']=='want_extra_type' else i+11
  if t=='number': return i+0.25
  if t=='boolean':return True
  if t=='reaper.array':return [0.0]*32
  if t=='string':return '' if p['name'].endswith('GUID') else 'input_'+str(i)
  return None
 args=[value(p,i) for i,p in enumerate(fn['parameters'])];inputs[n]=args
 results=[None]*len(fn['returns'])
 body=[]
 for j,p in enumerate(row['parameters']):
  var='p'+str(j);k=p['kind'];inp=p['input'];out=p['output'];v=args[inp] if inp>=0 else None
  if k in ('Length','LengthPtr'):
   buf=row['parameters'][p['link']];length=65536 if buf['kind']=='Buffer' else len(args[buf['input']] or '')
   body.append(f'CHECK({"*" if k=="LengthPtr" else ""}{var}=={length});')
  elif k in ('Int','UInt','Size','Bool','Double'):
   body.append(f'CHECK({var}=={json.dumps(v)});')
  elif k in ('IntPtr','UIntPtr','BoolPtr','DoublePtr'):
   body.append(f'CHECK({var} && *{var}=={json.dumps(v if inp>=0 else False if k=="BoolPtr" else 0)});')
   if out>=0:
    v= False if k=='BoolPtr' else j+0.5 if k=='DoublePtr' else j+301
    body.append(f'*{var}={json.dumps(v)};');results[out]=v
  elif k=='Handle':body.append(f'CHECK({var}==reinterpret_cast<{p["type"]}>({pointers[p["handle"]] if not p["flags"]&8 else 0}));')
  elif k=='HandleOut':body.append(f'CHECK({var} && *{var}==nullptr);')
  elif k in ('String','Buffer'):
   body.append(f'CHECK({var} && !std::strcmp({var},{json.dumps(v or "")}));')
   if out>=0:
    binary=bool(p['flags']&2);b='\\x00\\x90\\xff\\x7f' if binary else 'output_'+str(j)
    outsz=next((i for i,q in enumerate(row['parameters']) if q['kind']=='LengthPtr' and q['link']==j),None)
    body.append(f'std::memcpy({var},"{b}",{4 if binary else len(b)+1});')
    if outsz is not None:body.append(f'*p{outsz}={4 if binary else len(b)};')
    results[out]={'__reawebBytes':'AJD/fw=='} if binary else b
  elif k=='StringOut':body.append(f'*{var}="output_{j}";');results[out]='output_'+str(j)
  elif k=='Guid':
   body.append(f'CHECK({var} && {var}->Data1==0);')
   if out>=0:body.append(f'*{var}=guid;');results[out]='{12345678-1234-5678-0102-030405060708}'
  elif k=='Rect':
   for off,field in enumerate(['left','top','right','bottom']):body.append(f'CHECK({var}->{field}=={args[inp+off]});')
   if out>=0:
    for off,field in enumerate(['left','top','right','bottom']):body.append(f'{var}->{field}={401+off};');results[out+off]=401+off
  elif k=='Array':body.append(f'CHECK({var} && {var}[0]==0); {var}[0]=0.75; {var}[17]=-0.25;')
 ret=row['returnType'];rv=None;retexpr=''
 if ret!='void':
  if ret=='bool':rv=True;retexpr='true'
  elif ret in ('int','unsigned int'):rv=201;retexpr='201'
  elif ret=='double':rv=1.125;retexpr='1.125'
  elif ret=='const char*':rv='return';retexpr='"return"'
  elif ret=='GUID*':rv='{12345678-1234-5678-0102-030405060708}';retexpr='&guid'
  else:retexpr='nullptr'
  results[0]=rv
 outputs[n]=results[0] if len(results)==1 else results or None
 # Buffer length assertions must run before callees update them.
 checks=[x for x in body if x.startswith('CHECK')]; writes=[x for x in body if not x.startswith('CHECK')]
 # Array writes share their CHECK line and are safe.
 helpers=[]
 if n in ('GetSetMediaItemInfo_String','GetSetMediaItemTakeInfo_String','GetSetEnvelopeInfo_String'):
  helpers=['if(!std::strcmp(p1,"GUID")) {std::strcpy(p2,"{12345678-1234-5678-0102-030405060708}");return true;}']
 if n=='GetSetRegionOrMarkerInfo_String':
  helpers=['if(!std::strcmp(p2,"GUID")) {std::strcpy(p3,"{12345678-1234-5678-0102-030405060708}");return true;}']
 if n=='GetRegionOrMarker':helpers=[f'if(p1==-1) return reinterpret_cast<ProjectMarker*>({pointers["ProjectMarker"]});']
 lines += [f'{ret} mock_{fi}('+', '.join(p['type']+' p'+str(i) for i,p in enumerate(row['parameters']))+') {',f'++calls[{fi}];',*helpers,f'if(!std::strcmp(active,"{n}")) {{',*checks,*writes,'}']
 if retexpr:lines+=['return '+retexpr+';']
 lines+=['}']
lines+=['static void* resolve(const char* name) {','if(!std::strcmp(name,"realloc_cmd_register_buf")) return reinterpret_cast<void*>(register_buffer);','if(!std::strcmp(name,"realloc_cmd_clear")) return reinterpret_cast<void*>(clear_buffer);']
for i,row in enumerate(rows):lines += [f'if(!std::strcmp(name,"{row["name"]}")) return reinterpret_cast<void*>(mock_{i});']
lines+=['return nullptr;', '}', 'int main() { try {', 'Host host; host.native_function=resolve; host.current_project=[]()->void*{return nullptr;};', '']
for i,row in enumerate(rows):
 n=row['name'];args=inputs[n]
 # Avoid internal extra native helper invocation using a null take. The getter's mock requires null and is safe.
 lines += [f'active="{n}";', f'{{ NativeContext context(host,"abi"); auto args=Json::parse(R"ABI({json.dumps(args)})ABI");']
 for p in row['parameters']:
  if p['kind']=='Handle' and not p['flags']&8:
   typ=p['handle'];destroy='PCM_Source_Destroy' if typ=='PCM_source' else ''
   lines += [f'args[{p["input"]}]=context.handle(reinterpret_cast<void*>({pointers[typ]}),"{typ}",nullptr,"","{destroy}");']
 lines += [f'auto got=context.invoke(native_entries()[{i}],args);']
 if any(p['kind']=='Array' for p in row['parameters']):
  lines+=['CHECK(got["__reawebCall"]==true); CHECK(got["arrays"][0]["values"]["__reawebBytes"].is_string()); got=got["value"];']
 lines += [f'auto expected=Json::parse(R"ABI({json.dumps(outputs[n])})ABI"); if(got!=expected) {{ std::cerr<<active<<" got="<<got<<" expected="<<expected<<"\\n"; return 1; }} CHECK(calls[{i}]>=1); }}']
lines+=['std::cout<<"730/730 typed native ABI calls and complete argument/result mappings passed\\n"; return 0; } catch(const std::exception& e) {std::cerr<<e.what()<<"\\n"; return 1;} }']
(Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT/'tests/native_abi_generated.cpp').write_text('\n'.join(lines),encoding='utf-8')
