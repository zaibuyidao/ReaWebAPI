local source = debug.getinfo(1, "S").source
local folder = source:sub(2):match("^(.*[/\\])")
assert(reaper.APIExists("ReaWeb_OpenEx") and reaper.APIExists("ReaWeb_Show"), "ReaWebAPI v0.3.8.15 required")
local id = reaper.ReaWeb_OpenEx(folder .. "index.html", '{"hidden":true}')
assert(id > 0, reaper.ReaWeb_GetLastError())
local deadline = reaper.time_precise() + 30
local function wait()
  if not reaper.ReaWeb_IsOpen(id) then return end
  if reaper.ReaWeb_Receive(id) == "ui-ready" then
    assert(reaper.ReaWeb_Show(id), reaper.ReaWeb_GetLastError())
  elseif reaper.time_precise() >= deadline then
    reaper.ReaWeb_Close(id)
    error("App preload timed out")
  else
    reaper.defer(wait)
  end
end
wait()
