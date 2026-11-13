local source = debug.getinfo(1, 'S').source
local root = source:sub(2):match('^(.*[/\\])')
reaper.ReaWeb_Open(root .. 'index.html', source)
