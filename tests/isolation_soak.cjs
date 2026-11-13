// Opt-in WebView2 acceptance with CDP Worker counts and Windows process memory samples.
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const { spawn, execFile } = require('node:child_process');
const { promisify } = require('node:util');
const execute = promisify(execFile);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function main() {
  const [dll, fixture, output, duration = '1800', reopen = '0', core = 'dosbox_pure', pspProgram] = process.argv.slice(2);
  if (!dll || !fixture || !output) throw new Error('Usage: node tests/isolation_soak.cjs DLL FIXTURE NEW_OUTPUT [SECONDS]');
  const root = path.resolve(output);
  fs.mkdirSync(root, { recursive: false });
  const listener = net.createServer();
  await new Promise(resolve => listener.listen(0, '127.0.0.1', resolve));
  const port = listener.address().port;
  await new Promise(resolve => listener.close(resolve));
  const log = fs.openSync(path.join(root, 'host.log'), 'w');
  const child = spawn('python', ['-X', 'utf8', 'tests/plugin_smoke.py', '--dll', dll, '--webview',
    '--isolation-emulator', fixture, '--isolation-core', core, '--isolation-soak-seconds', duration,
    ...(pspProgram ? ['--isolation-psp-program', pspProgram] : []),
    '--isolation-reopen', reopen, '--idle-webviews', '0', '--resource-root', root], {
    windowsHide: true, stdio: ['ignore', log, log], env: {
      ...process.env, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port}`
    }
  });
  let done = false, exitCode, socket;
  child.on('exit', code => { done = true; exitCode = code; });
  const samples = [], started = Date.now();
  try {
    let endpoint;
    for (let i = 0; i < 200 && !done; ++i) {
      try { endpoint = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json(); break; }
      catch { await sleep(100); }
    }
    if (!endpoint) throw new Error('WebView2 debugging endpoint did not start');
    socket = new WebSocket(endpoint.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
    let sequence = 0;
    const pending = new Map();
    socket.onmessage = ({ data }) => {
      const message = JSON.parse(data), item = pending.get(message.id);
      if (item) { pending.delete(message.id); message.error ? item.reject(message.error) : item.resolve(message.result); }
    };
    socket.onclose = () => { for (const item of pending.values()) item.reject(new Error('Browser closed')); pending.clear(); };
    const send = (method, params = {}) => new Promise((resolve, reject) => {
      const id = ++sequence;
      const timeout = setTimeout(() => { pending.delete(id); reject(new Error('Browser request timed out')); }, 3000);
      pending.set(id, { resolve: value => { clearTimeout(timeout); resolve(value); }, reject: error => { clearTimeout(timeout); reject(error); } });
      socket.send(JSON.stringify({ id, method, params }));
    });
    while (!done) {
      try {
        const [{ targetInfos }, { processInfo }] = await Promise.all([send('Target.getTargets'), send('SystemInfo.getProcessInfo')]);
        const ids = [child.pid, ...processInfo.map(p => p.id)].filter(id => Number.isSafeInteger(id) && id > 0);
        const { stdout } = await execute('powershell.exe', ['-NoProfile', '-Command',
          `Get-Process | Where-Object { $_.Id -in @(${ids.join(',')}) -and ($_.ProcessName -eq 'msedgewebview2' -or ($_.Id -eq ${child.pid} -and $_.ProcessName -like 'python*')) } | Select-Object Id,ProcessName,WorkingSet64,PrivateMemorySize64,@{Name='StartTimeTicks';Expression={$_.StartTime.ToUniversalTime().Ticks.ToString()}} | ConvertTo-Json -Compress`], { windowsHide: true });
        const processes = [].concat(JSON.parse(stdout));
        samples.push({ seconds: (Date.now() - started) / 1000,
          workers: targetInfos.filter(t => t.type === 'worker').length,
          targets: targetInfos.map(t => ({ type: t.type, url: t.url })), processes,
          privateBytes: processes.reduce((sum, p) => sum + p.PrivateMemorySize64, 0),
          workingSetBytes: processes.reduce((sum, p) => sum + p.WorkingSet64, 0) });
        fs.writeFileSync(path.join(root, 'stability-samples.json'), JSON.stringify({ endpoint, samples }, null, 2));
        if (samples.length % 6 === 0) console.log(JSON.stringify(samples.at(-1)));
      } catch (error) {
        if (!done && Number(reopen)) {
          socket.close();
          try {
            endpoint = await (await fetch(`http://127.0.0.1:${port}/json/version`, { signal: AbortSignal.timeout(2000) })).json();
            const previousHandler = socket.onmessage, previousClose = socket.onclose;
            socket = new WebSocket(endpoint.webSocketDebuggerUrl);
            await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; setTimeout(() => reject(new Error('Browser reconnection timed out')), 2000); });
            socket.onmessage = previousHandler; socket.onclose = previousClose;
          } catch { /* No browser is expected while all App windows are closed. */ }
          samples.push({ seconds: (Date.now() - started) / 1000, monitoringGap: String(error) });
        } else if (!done) throw error;
      }
      for (let i = 0; i < (Number(reopen) ? 10 : 100) && !done; ++i) await sleep(100);
    }
    console.log(JSON.stringify({ exitCode, samples: samples.length, elapsedSeconds: (Date.now() - started) / 1000 }));
    const identity = process => `${process.Id}:${process.ProcessName}:${process.StartTimeTicks}`;
    const observedProcesses = samples.flatMap(sample => sample.processes || []);
    const identities = new Set(observedProcesses.map(identity));
    const observed = [...new Set(observedProcesses.map(process => process.Id))];
    await sleep(2000);
    const { stdout } = await execute('powershell.exe', ['-NoProfile', '-Command',
      `@(Get-Process | Where-Object { $_.Id -in @(${observed.join(',')}) } | Select-Object Id,ProcessName,@{Name='StartTimeTicks';Expression={$_.StartTime.ToUniversalTime().Ticks.ToString()}}) | ConvertTo-Json -Compress`], { windowsHide: true });
    const current = stdout.trim() ? [].concat(JSON.parse(stdout)) : [];
    const remaining = current.filter(process => identities.has(identity(process)));
    const active = samples.filter(sample => sample.workers > 0);
    const steady = active.filter(sample => sample.seconds >= Math.min(300, Number(duration) / 4));
    const average = values => values.reduce((sum, value) => sum + value, 0) / values.length;
    const firstPrivate = average(steady.slice(0, 5).map(sample => sample.privateBytes));
    const lastPrivate = average(steady.slice(-5).map(sample => sample.privateBytes));
    fs.writeFileSync(path.join(root, 'stability-summary.json'), JSON.stringify({
      exitCode, elapsedSeconds: (Date.now() - started) / 1000, samples: samples.length,
      monitoringGaps: samples.filter(sample => sample.monitoringGap).length,
      activeWorkerCounts: [...new Set(active.map(sample => sample.workers))],
      firstPrivateBytes: firstPrivate, lastPrivateBytes: lastPrivate,
      privateGrowthBytes: lastPrivate - firstPrivate, remainingProcesses: remaining,
      reusedProcessIds: current.filter(process => !identities.has(identity(process)))
    }, null, 2));
    if (exitCode !== 0) throw new Error(`Core acceptance failed, see ${root}/host.log`);
    if (remaining.length) throw new Error(`Test processes remain, see ${root}/stability-summary.json`);
  } finally {
    socket?.close();
    if (!done) child.kill();
    fs.closeSync(log);
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
