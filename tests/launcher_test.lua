local root = arg[1] or '.'
local launchers = {
  'examples/main-demo/ReaWebAPI_Demo.lua', 'examples/templates/starter/reawebapi-starter.lua', 'examples/runtime-features/runtime-demo/reawebapi-runtime-demo.lua',
  'examples/templates/modern/reawebapi-modern.lua', 'examples/templates/modern/reawebapi-modern-dev.lua', 'examples/runtime-features/web-runtime/reawebapi-web-runtime.lua',
  'examples/backends/lua-backend/reawebapi-lua-backend.lua', 'examples/integrations/native-service/reawebapi-native-service.lua'
}
for _, path in ipairs(launchers) do
  local ext, windows, deferred, exits, sent, instances = {}, {}, {}, {}, {}, {}
  local created, focused, current, volume = 0, 0, 0, 1
  local function open(_, instanceKey)
    if instanceKey then
      assert(instanceKey:sub(1, 1) == '@' and instanceKey:find(path, 1, true), 'Pass the launcher source as instanceKey')
      local id = instances[instanceKey]
      if id and windows[id] then focused = focused + 1 return id end
    end
    created = created + 1
    windows[created] = true
    if instanceKey then instances[instanceKey] = created end
    return created
  end
  reaper = {
    APIExists = function() return true end,
    GetExtState = function(section, key)
      assert(path == 'examples/templates/modern/reawebapi-modern-dev.lua' or section == 'ReaWebAPI.Backends', 'Window reuse belongs to Runtime')
      return ext[section .. key] or ''
    end,
    SetExtState = function(section, key, value, persist)
      assert(not persist, 'Window IDs must not survive REAPER restarts')
      ext[section .. key] = value
    end,
    DeleteExtState = function(section, key) ext[section .. key] = nil end,
    ReaWeb_Open = open, ReaWeb_OpenDev = open,
    ReaWeb_IsOpen = function(id) return windows[id] or false end,
    ReaWeb_IsReady = function(id) return windows[id] or false end,
    ReaWeb_Close = function(id) windows[id] = nil return true end,
    ReaWeb_Focus = function(id) assert(windows[id]) focused = focused + 1 end,
    ReaWeb_Send = function(id, message) assert(windows[id]) sent[#sent + 1] = message return true end,
    ReaWeb_Receive = function() return '' end,
    GetSelectedTrack = function() return 1 end,
    GetTrackName = function() return true, '鼓组 "A"\n\t\\' end,
    GetMediaTrackInfo_Value = function() return volume end,
    SetMediaTrackInfo_Value = function(_, _, value) volume = value end,
    UpdateArrange = function() end,
    time_precise = function() return 1 end,
    defer = function(callback) deferred[current] = callback end,
    atexit = function(callback) exits[current] = callback end,
    MB = function(message) error(message) end,
    ShowMessageBox = function(message) error(message) end,
  }
  local function run()
    current = current + 1
    assert(loadfile(root .. '/' .. path))()
  end
  run()
  assert(created == 1 and windows[1], path)
  run()
  assert(created == 1 and focused == 1, 'Duplicate launcher: ' .. path)
  assert(not deferred[2] and not exits[2], 'Duplicate backend must not own a loop or cleanup')
  windows[1] = nil -- Native Docker close makes IsOpen false.
  run()
  assert(created == 2 and windows[2], 'Relaunch failed: ' .. path)
  if path == 'examples/backends/lua-backend/reawebapi-lua-backend.lua' then
    assert(sent[1] == [=[{"type":"state","track":{"name":"鼓组 \"A\"\u000a\u0009\\","volume":1.0000000000}}]=], sent[1])
    local old = deferred[1]; deferred[1] = nil; current = 1; old()
    assert(not deferred[1], 'Closed Docker must stop the Lua receive loop')
    exits[1]()
    assert(windows[2], 'Old cleanup closed the replacement window')
    current = 3
    local messages = {'volume nope', 'volume 3', 'volume 0.5'}
    reaper.ReaWeb_Receive = function() return table.remove(messages, 1) or '' end
    deferred[3]()
    assert(volume == 0.5)
    exits[3]()
    assert(not windows[2] and next(ext) == nil, 'Backend termination must release ownership')
  end
end
for _, name in ipairs({'reawebapi-frameless-resizable.lua', 'reawebapi-frameless-fixed.lua'}) do
  local called = false
  reaper = {
    APIExists = function(api) return api == 'ReaWeb_OpenEx' end,
    ReaWeb_OpenEx = function(path, options)
      assert(path:match('/frameless/index.html$'), path)
      assert(options:find('"frameless":true', 1, true), options)
      assert(options:find('"resizable":' .. tostring(name == 'reawebapi-frameless-resizable.lua'), 1, true), options)
      assert(options:find(name, 1, true) and options:find('"instanceKey":"@', 1, true), options)
      called = true; return 1
    end,
    MB = function(message) error(message) end,
  }
  assert(loadfile(root .. '/examples/windows/frameless/' .. name))()
  assert(called, name)
end
do
  local message
  reaper = { MB = function(text) message = text end }
  assert(loadfile(root .. '/examples/windows/frameless/launch.lua'))()
  assert(message and message:find('reawebapi-frameless-resizable.lua', 1, true) and message:find('reawebapi-frameless-fixed.lua', 1, true),
    'Running the shared helper directly must explain which launcher to use')
end
print('Lua launchers: singleton, focus, close, restart, backend cleanup and message encoding passed')
