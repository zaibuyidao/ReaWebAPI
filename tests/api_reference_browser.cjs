/* Node 22+ and Chrome/Chromium. Drive a standalone file from a local test page. */
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { pathToFileURL } = require('node:url');

async function exercise(frame) {
  const w = frame.contentWindow, doc = frame.contentDocument;
  const el = id => doc.getElementById(id);
  const check = (condition, message) => { if (!condition) throw Error(message); };
  const until = async predicate => {
    for (let i = 0; i < 100 && !predicate(); i++) await new Promise(resolve => setTimeout(resolve, 10));
    check(predicate(), 'Navigation timed out');
  };
  const target = () => el(w.location.hash.slice(1));
  const heading = () => target()?.querySelector('h3')?.firstChild.textContent;
  const visibleIndex = () => el('index').querySelectorAll('section:not([hidden]) a:not([hidden])');
  const query = text => { el('query').value = text; el('query').dispatchEvent(new w.Event('input')); };
  check(heading() === 'reaper.realtime.subscribe', 'Direct anchor');
  const {entries} = JSON.parse(el('reference-data').textContent);
  const ids = new Set(entries.map(e => e.id));
  check(ids.size === entries.length && entries.every(e => e.related.every(id => ids.has(id))), 'Internal links');
  check(doc.querySelectorAll('article:not([hidden])').length === entries.length, 'Every entry in the document');
  check([...ids].every(id => el(id)?.matches('article')), 'Real entry anchors');
  check([...doc.querySelectorAll('article')].every(a => a.querySelector('p:not(.related)')), 'Description for each entry');
  const open = el('host.ReaWeb_Open');
  check(open.querySelectorAll('.line').length === 5 && open.innerText.includes('instanceKey'), 'Host bindings and semantics');
  el('views').querySelector('[value="Lua"]').click();
  check(!open.hidden && open.querySelectorAll('.line:not([hidden])').length === 1 && el('runtime.lifecycle.ready').hidden, 'Language view');
  el('views').querySelector('[value=""]').click();
  check(!doc.querySelector('script[src],link[rel="stylesheet"],iframe,img[src]'), 'External page assets');
  check(doc.documentElement.scrollWidth <= w.innerWidth, 'Narrow layout');
  check(target().querySelectorAll('.line').length === 3, 'Overload signatures');
  for (const language of ['en','zh-CN','en']) {
    if (doc.documentElement.lang !== language) el('language').click();
    check(doc.documentElement.lang === language, 'Document language');
    const articles = [...doc.querySelectorAll('article')];
    check(articles.every(a => [...a.querySelectorAll('p:not(.related)')].some(p => p.getClientRects().length)), 'Visible description for every entry');
    if (language === 'en') {
      check(!/\p{Script=Han}/u.test(el('reference').innerText), 'Chinese text in English reference');
      check(el('runtime.audio.getTrackMix').innerText.includes('linear gain'), 'English inventory description');
    } else {
      check(articles.every(a => [...a.querySelectorAll('p.en,.en')].every(p => !p.getClientRects().length)), 'English prose in Chinese reference');
      check(el('runtime.audio.getTrackMix').innerText.includes('线性音量'), 'Chinese inventory description');
      const stream = el('type.ReaWebStream').querySelector('code').innerText;
      check(stream.includes('消费者缓存') && !stream.includes('cached data'), 'Localized type comments');
      check(el('service.jsfx.beginParamEdit').innerText.includes('撤销手势'), 'Localized native service');
    }
  }
  const hostIndex = doc.querySelector('[data-group="host"] .columns');
  function columnStarts(width) {
    frame.width = width;
    doc.body.getBoundingClientRect();
    return [...new Set([...hostIndex.querySelectorAll('a:not([hidden])')].map(a => a.getBoundingClientRect().left))];
  }
  const wide = columnStarts(1440);
  check(wide.length === 4 && String(wide) === String(columnStarts(1920)), 'Compact four-column desktop index');
  const links = [...hostIndex.children];
  const rows = Math.ceil(links.length / 4);
  const gap = parseFloat(w.getComputedStyle(hostIndex).columnGap);
  check(wide[0] === hostIndex.getBoundingClientRect().left && Math.abs(wide[1] - links[0].getBoundingClientRect().right - gap) < 1, 'Left alignment and column gap');
  check(links[rows].getBoundingClientRect().top === links[0].getBoundingClientRect().top, 'Column-major order');
  check(columnStarts(760).length === 2 && columnStarts(375).length === 1, 'Responsive index columns');
  const theme = doc.documentElement.dataset.theme;
  el('theme').click(); check(doc.documentElement.dataset.theme !== theme, 'Theme switch');
  query('REAPER.WINDOW.GETSTATE');
  check([...visibleIndex()].some(a => a.hash === '#runtime.window.getState'), 'Case-insensitive search');
  el('query').dispatchEvent(new w.KeyboardEvent('keydown',{key:'Enter'}));
  await until(() => heading() === 'reaper.window.getState');
  w.history.back(); await until(() => heading() === 'reaper.realtime.subscribe');
  w.history.forward(); await until(() => heading() === 'reaper.window.getState');
  let copied;
  Object.defineProperty(w.navigator, 'clipboard', {configurable:true,value:{writeText:async value => { copied = value; }}});
  target().querySelector('.line button').click();
  await until(() => copied === target().querySelector('.line code').innerText);
  Object.defineProperty(w.navigator, 'clipboard', {configurable:true,value:undefined});
  target().querySelector('[data-copy="link"]').click();
  check(target().querySelector('textarea').value.endsWith('#runtime.window.getState'), 'Copy fallback');
  query('停靠'); check(visibleIndex().length > 0, 'Chinese search');
  query('no-such-entry-xyz'); check(visibleIndex().length === 0, 'Empty result');
  el('clear').click(); check(el('query').value === '', 'Clear');
  el('filter').value = 'host'; el('filter').dispatchEvent(new w.Event('change'));
  check(visibleIndex().length === entries.filter(e => e.group === 'host').length, 'Group filter');
  check(!w.performance.getEntriesByType('resource').some(r => /^https?:/.test(r.name)), 'HTTP dependency');
}

const root = fs.mkdtempSync(path.join(os.tmpdir(),'reaweb-reference-'));
try {
  const folder = path.join(root,'离线 手册');
  fs.mkdirSync(folder);
  fs.copyFileSync(process.argv[2],path.join(folder,'reawebapihelp.html'));
  const driver = path.join(root,'test.html');
  // Keep the driver's load event pending until the asynchronous assertions settle.
  // Headless --dump-dom then captures the result on the browser's real clock.
  fs.writeFileSync(driver, `<!doctype html><meta charset="utf-8"><body>
    <iframe hidden id="pending"></iframe><output>Tests did not finish</output>
    <script>
      const pending = document.getElementById('pending').contentDocument;
      pending.open();
      pending.write('<title>Running tests</title>');
      const exercise = ${exercise.toString()};
      async function run(frame) {
        const output = document.querySelector('output');
        try {
          await exercise(frame);
          output.textContent = 'PASS';
        } catch (error) {
          output.textContent = error.stack || String(error);
        } finally {
          pending.close();
        }
      }
    </script>
    <iframe width="375" height="680" onload="run(this)"
      src="./${path.basename(folder)}/reawebapihelp.html#runtime.realtime.subscribe"></iframe>`);
  const chrome = process.argv[3] || process.env.CHROME_BIN || 'google-chrome';
  const result = spawnSync(chrome, ['--headless','--disable-gpu','--no-first-run',
    '--disable-background-networking','--allow-file-access-from-files','--blink-settings=localStorageEnabled=false',
    '--host-resolver-rules=MAP * ~NOTFOUND','--user-data-dir='+path.join(root,'profile'),'--timeout=30000',
    '--dump-dom',pathToFileURL(driver).href],{encoding:'utf8',windowsHide:true,timeout:45000});
  if (result.error) {
    throw new Error(`Cannot complete the browser test with ${chrome}: ${result.error.message}\n${result.stderr || ''}`, {cause:result.error});
  }
  assert.equal(result.status,0,result.stderr);
  const report = result.stdout.match(/<output>([\s\S]*?)<\/output>/);
  assert.equal(report?.[1],'PASS', report?.[1] || result.stderr);
  console.log('PASS: standalone file, overloads, search, history, links, copy, theme, language and narrow layout.');
} finally {
  fs.rmSync(root,{recursive:true,force:true,maxRetries:10,retryDelay:150});
}
