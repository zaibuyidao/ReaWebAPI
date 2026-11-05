-- @noindex
local reaper = reaper

for _, name in ipairs({
  'ReaWeb_OpenEx', 'ReaWeb_Close', 'ReaWeb_IsOpen', 'ReaWeb_IsReady',
  'ReaWeb_Send', 'ReaWeb_Receive', 'ReaWeb_SetFrame', 'ReaWeb_SetShortcutRouting',
}) do
  if not reaper.APIExists(name) then
    reaper.MB('Install ReaWebAPI v0.3.9.26 or later and restart REAPER.', 'ReaWebAPI', 0)
    return
  end
end

local directory = debug.getinfo(1, 'S').source:sub(2):match('^(.*[/\\])')
local instance = 'reawebapi-overlay-demo'
local id = reaper.ReaWeb_OpenEx(directory .. 'index.html',
  '{"instanceKey":"reawebapi-overlay-demo","frameless":true,"transparent":true,"resizable":true,"layer":"overlay","mouseMode":"regions"}')
if id == 0 then
  reaper.MB(reaper.ReaWeb_GetLastError(), 'ReaWebAPI', 0)
  return
end

reaper.SetExtState('ReaWebAPI.OverlayDemo', 'window', tostring(id), false)
local owners = 'ReaWebAPI.Backends'
if tonumber(reaper.GetExtState(owners, instance)) == id then return end
reaper.SetExtState(owners, instance, tostring(id), false)

reaper.atexit(function()
  if reaper.GetExtState(owners, instance) == tostring(id) then
    reaper.ReaWeb_Close(id)
    reaper.DeleteExtState(owners, instance, false)
  end
  if reaper.GetExtState('ReaWebAPI.OverlayDemo', 'window') == tostring(id) then
    reaper.DeleteExtState('ReaWebAPI.OverlayDemo', 'window', false)
  end
end)

function finite(value)
  return type(value) == 'number' and value == value and value ~= math.huge and value ~= -math.huge
end

local escapes = { ['"'] = '\\"', ['\\'] = '\\\\' }

function json(value)
  local kind = type(value)

  if kind == 'string' then
    return '"' .. value:gsub('[%z\1-\31\\"]', function(c)
      return escapes[c] or string.format('\\u%04x', c:byte())
    end) .. '"'
  end

  if kind == 'boolean' then return tostring(value) end
  if kind == 'number' then
    assert(finite(value), 'Cannot send a non-finite value.')
    return (string.format('%.17g', value):gsub(',', '.'))
  end

  if kind == 'nil' then return 'null' end
  assert(kind == 'table', 'Expected plain data.')
  local keys, parts = {}, {}

  for key in pairs(value) do keys[#keys + 1] = key end
  table.sort(keys)
  for _, key in ipairs(keys) do parts[#parts + 1] = json(key) .. ':' .. json(value[key]) end
  return '{' .. table.concat(parts, ',') .. '}'
end

local project, master_guid, track, track_guid
local epoch, selection, revision = 0, 0, 0
local session, previous

function context()
  local current = reaper.EnumProjects(-1)
  local master = reaper.GetTrackGUID(reaper.GetMasterTrack(current))

  if current ~= project or master ~= master_guid then
    project, master_guid = current, master
    epoch, selection = epoch + 1, selection + 1
    track, track_guid = nil, nil
  end

  local selected = reaper.GetSelectedTrack(project, 0)
  local guid = selected and reaper.GetTrackGUID(selected) or nil

  if selected ~= track or guid ~= track_guid then
    track, track_guid = selected, guid
    selection = selection + 1
  end
end

function snapshot()
  context()

  local state = { session = session, epoch = epoch, selection = selection, revision = revision, track = false }

  if track and reaper.ValidatePtr2(project, track, 'MediaTrack*') then
    local _, name = reaper.GetTrackName(track)
    state.track = {
      id = track_guid, name = name,
      volume = reaper.GetMediaTrackInfo_Value(track, 'D_VOL'),
      pan = reaper.GetMediaTrackInfo_Value(track, 'D_PAN'),
    }
  end

  return state
end

local continuous = { volume = 'D_VOL', pan = 'D_PAN' }

function execute(request)
  context()
  if request.command == 'ready' then return {} end
  assert(request.epoch == epoch, 'Project changed. Try again on the current track.')

  if request.command == 'play' then
    reaper.Main_OnCommandEx(40044, 0, project)
    return {}
  end

  local parameter = continuous[request.command]
  assert(parameter, 'Unknown overlay command.')
  assert(request.selection == selection, 'Track selection changed. Try again on the current track.')
  assert(track and reaper.ValidatePtr2(project, track, 'MediaTrack*'), 'Select a track first.')
  local value = tonumber(request.value)
  assert(finite(value), 'Expected a finite parameter value.')
  assert(request.command == 'volume' and value >= 0 and value <= 4 or
    request.command == 'pan' and value >= -1 and value <= 1, 'Parameter value is out of range.')
  assert(reaper.SetMediaTrackInfo_Value(track, parameter, value), 'REAPER rejected the parameter update.')
  reaper.UpdateArrange()
  local actual = reaper.GetMediaTrackInfo_Value(track, parameter)
  assert(math.abs(actual - value) < 0.000001, 'Parameter readback differs from the requested value.')
  revision = revision + 1
  return { value = actual, revision = revision }
end

function parse(message)
  local client, request_id, command, requested_epoch, requested_selection, value = message:match('^([%w%-]+)\t(%d+)\t([%w%-]+)\t(%d+)\t(%d+)\t(.*)$')
  if not client then return end

  return { client = client, id = tonumber(request_id), command = command,
    epoch = tonumber(requested_epoch), selection = tonumber(requested_selection), value = value }
end

function handle(request, superseded)
  if request.command == 'ready' then
    session, previous = request.client, nil
  elseif request.client ~= session then
    return
  end

  local response = { type = 'response', session = request.client, id = request.id, ok = true }

  if superseded then
    response.data = { superseded = true }
  else
    local ok, result = pcall(execute, request)
    response.ok = ok
    if ok then response.data = result else response.error = tostring(result) end
    if not ok or not continuous[request.command] then response.state = snapshot() end
  end

  assert(reaper.ReaWeb_Send(id, json(response)), reaper.ReaWeb_GetLastError())
end

function tick()
  local batch, newest = {}, {}

  for _ = 1, 32 do
    local message = reaper.ReaWeb_Receive(id)
    if message == '' then break end
    local request = parse(message)
    if request then
      batch[#batch + 1] = request
      if continuous[request.command] then
        request.key = table.concat({ request.client, request.epoch, request.selection, request.command }, ':')
        newest[request.key] = #batch
      end
    end
  end

  for index, request in ipairs(batch) do
    handle(request, request.key and newest[request.key] ~= index)
  end

  if session and reaper.ReaWeb_IsReady(id) then
    local encoded = json(snapshot())
    if encoded ~= previous and reaper.ReaWeb_SetFrame(id, encoded) then previous = encoded end
  end
end

function loop()
  if not reaper.ReaWeb_IsOpen(id) then return end
  local ok, err = pcall(tick)
  if not ok then
    reaper.MB('Overlay Lua backend stopped:\n' .. tostring(err), 'ReaWebAPI', 0)
    return
  end
  reaper.defer(loop)
end

loop()
