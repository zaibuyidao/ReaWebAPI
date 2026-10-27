local root=reaper.GetResourcePath()..'/'
local id=reaper.ReaWeb_OpenEx(root..'app/index.html','{"hidden":true}')
local sizes,index,wanted={1024,16384,65536,1048576},1,0
local function log(kind,actual)
  local f=assert(io.open(root..'result.txt','a'))
  f:write(kind..'\t'..wanted..'\t'..tostring(actual)..'\n');f:close()
end
local function next_size()
  wanted=sizes[index]
  local text=string.rep(' ',wanted)
  log('BUILD',#text);log('SEND_ACCEPTED',reaper.ReaWeb_Send(id,text) and 1 or 0)
end
local started,finished=reaper.time_precise(),false
local function poll()
  if reaper.time_precise()-started>30 then log('TIMEOUT',0);reaper.ReaWeb_Close(id);return end
  for _=1,16 do
    local message=reaper.ReaWeb_Receive(id)
    if message=='ready' then next_size() end
    local length=message:match('^length:(%d+)$')
    if length then log('OUTBOUND',length);reaper.ReaWeb_Send(id,'bytes:'..wanted) end
    if message:sub(1,1)=='x' then
      log('INBOUND',#message);index=index+1
      if index<=#sizes then next_size() else finished=true end
    end
  end
  if finished then
    reaper.ReaWeb_Close(id)
    local f=assert(io.open(root..'result.txt','a'));f:write('COMPLETE\n');f:close()
  else reaper.defer(poll) end
end
poll()
