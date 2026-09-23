"""Verify native Lua instance options in a disposable REAPER resource directory."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time


SCRIPT = r'''
local source = debug.getinfo(1, 'S').source
local root = source:sub(2):match('^(.*[/\\])')
local page = root .. 'Scripts/Tool/index.html'
local ids, commands = {}, {}
local finished = false
local function check(value, message)
  if not value then error(message or reaper.ReaWeb_GetLastError(), 2) end
  return value
end
local function remember(id)
  check(id > 0)
  ids[#ids + 1] = id
  return id
end
local function open(...)
  return remember(reaper.ReaWeb_Open(...))
end
local function action(path)
  local command = check(reaper.AddRemoveReaScript(true, 0, root .. path, true))
  check(command > 0)
  commands[#commands + 1] = path
  return function()
    reaper.Main_OnCommand(command, 0)
    return remember(tonumber(reaper.GetExtState('ReaWebAPI.InstanceTest', 'id')) or 0)
  end
end
local function finish(message)
  if finished then return end
  finished = true
  for _, id in ipairs(ids) do reaper.ReaWeb_Close(id) end
  for _, path in ipairs(commands) do reaper.AddRemoveReaScript(false, 0, root .. path, true) end
  reaper.DeleteExtState('ReaWebAPI.InstanceTest', 'id', false)
  local report = assert(io.open(root .. 'result.txt', 'w'))
  report:write(message)
  report:close()
end
local function start()
  check(reaper.GetResourcePath():gsub('[/\\]+$', '') == root:gsub('[/\\]+$', ''), 'Expected isolated resource directory')
  local run = action('Scripts/Tool/Open.lua')
  local first = run()
  check(run() == first, 'Fresh Lua invocation did not reuse the initializing window')
  check(action('Scripts/Copy/Open.lua')() ~= first, 'Copied tool collided')
  check(action('Scripts/Tool/Other.lua')() ~= first, 'Different launchers collided')
  local main = open(page, source)
  check(open(page, source, nil, false) == main, 'Explicit false bypassed reuse')
  local settings = open(page, source, 'Settings')
  check(settings ~= main and open(page, source, 'Settings') == settings, 'Named window identity')
  check(open(page, source, 'settings') ~= settings, 'Names must match exactly')
  check(open(page, source .. '/different') ~= main, 'Keys must match exactly')
  check(open(page) ~= open(page), 'Missing key must create independent windows')
  check(open(page, nil) ~= open(page, ''), 'Nil and empty keys must create independent windows')
  check(open(page, source, nil, true) ~= open(page, source, nil, true), 'multiple=true must bypass reuse')
  check(open(page, source) == main, 'Multiple windows replaced the singleton')
  local ok = pcall(reaper.ReaWeb_Open, {url = page, instanceKey = source})
  check(not ok, 'Unexpected table support: review the documented positional contract')
  check(reaper.ReaWeb_Close(main), 'Close failed')
  local replacement = open(page, source)
  check(replacement ~= main, 'Closing window was reused')
  local deadline = reaper.time_precise() + 40
  local function wait_ready()
    local success, message = pcall(function()
      check(reaper.time_precise() < deadline, 'WebView readiness timed out')
      for _, id in ipairs(ids) do
        if id ~= main then
          check(reaper.ReaWeb_IsOpen(id), 'Window failed before readiness: ' .. id)
          if not reaper.ReaWeb_IsReady(id) then reaper.defer(wait_ready) return end
        end
      end
      check(run() == first, 'Ready window was not reused')
      check(open(root .. 'missing.html', source) == replacement, 'Reuse navigated to a new path')
      check(reaper.ReaWeb_IsReady(replacement), 'Reuse lost the ready document')
      finish('PASS: fresh Lua states, explicit source keys, copied/different launchers, named windows, no key, multiple, initializing/ready reuse and close/reopen')
    end)
    if not success then finish('FAIL: ' .. tostring(message)) end
  end
  reaper.defer(wait_ready)
end
local ok, message = pcall(start)
if not ok then finish('FAIL: ' .. tostring(message)) end
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reaper', type=Path, required=True)
    parser.add_argument('--extension', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    (root / 'UserPlugins').mkdir()
    (root / 'EmptyPlugins').mkdir()
    (root / 'reaper.ini').write_text(
        '[REAPER]\naudiomode=4\nloadlastproj=0\nsplash=0\nnewprojdo=0\n'
        f'vstpath64={root / "EmptyPlugins"}\nvstfullstate=49989\n', encoding='utf-8')
    binary = root / 'UserPlugins' / args.extension.name
    shutil.copy2(args.extension, binary)
    if sys.platform == 'darwin':
        subprocess.run(['codesign', '--force', '--sign', '-', str(binary)], check=True)
    elif sys.platform.startswith('linux'):
        for helper in args.extension.parent.glob('reawebapi-webview-*'):
            shutil.copy2(helper, root / 'UserPlugins' / helper.name)
    launcher = '''local instanceKey = debug.getinfo(1, "S").source
local directory = instanceKey:sub(2):match("^(.*[/\\\\])")
local function open() return reaper.ReaWeb_Open(directory .. 'index.html', instanceKey) end
reaper.SetExtState('ReaWebAPI.InstanceTest', 'id', tostring(open()), false)
'''
    for folder in ('Tool', 'Copy'):
        path = root / 'Scripts' / folder
        path.mkdir(parents=True)
        (path / 'index.html').write_text('<!doctype html><title>Instance test</title>', encoding='utf-8')
        for name in ('Open.lua', 'Other.lua'):
            (path / name).write_text(launcher, encoding='utf-8')
    script = root / 'single_instance_smoke.lua'
    script.write_text(SCRIPT, encoding='utf-8')
    command = [str(args.reaper.resolve()), '-newinst', '-cfgfile', str(root / 'reaper.ini'),
               '-new', '-nosplash', str(script)]
    with (root / 'process.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen(command, stdout=log, stderr=log)
        try:
            report = root / 'result.txt'
            deadline = time.monotonic() + 60
            while not report.exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.2)
            result = report.read_text(encoding='utf-8') if report.exists() else 'FAIL: no result'
            print(json.dumps({'resource': str(root), 'result': result}, ensure_ascii=False), flush=True)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    return 0 if result.startswith('PASS:') else 1


if __name__ == '__main__':
    raise SystemExit(main())
