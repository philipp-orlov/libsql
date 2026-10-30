// The interactive parts of the report features in a real browser, with the real mouse (DevTools Input events): the card that
// follows the pointer over a bar, the highlight of a category in every chart, a click that selects it, the legend that
// switches a series off, a table sorted by a click on its heading and a group folded by a click on its heading; in the
// designer, a click on an item of the preview that opens it, a field dragged onto a shelf, and undo.
//   node scripts/interactive-e2e.mjs path/to/sql path/to/chrome path/to/demo
import { spawn } from 'node:child_process';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const [binary, chrome, demo] = process.argv.slice(2);
assert(binary && chrome && demo, 'usage: node scripts/interactive-e2e.mjs path/to/sql path/to/chrome path/to/demo');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const freePort = () => new Promise(res => { const p = net.createServer().listen(0, '127.0.0.1', () => { const { port } = p.address(); p.close(() => res(port)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'sql-interactive-'));
fs.copyFileSync(path.join(demo, 'sales_demo.sqldb'), path.join(dir, 'sales_demo.sqldb'));
fs.cpSync(path.join(demo, 'sales_demo.sqldb.reports'), path.join(dir, 'sales_demo.sqldb.reports'), { recursive: true });
const port = await freePort();
const server = spawn(binary, [path.join(dir, 'sales_demo.sqldb'), '--listen', String(port)], { stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise((resolve, reject) => { server.stdout.once('data', resolve); server.on('exit', c => reject(new Error(`the server exited with ${c}`))); });
const debugPort = await freePort();
const browser = spawn(chrome, [`--remote-debugging-port=${debugPort}`, '--headless=new', '--no-sandbox', '--disable-gpu', `--user-data-dir=${dir}/profile`, '--window-size=1500,1050', 'about:blank'], { stdio: 'ignore' });
const finish = async code => {
  const closed = new Promise(resolve => browser.once('exit', resolve));
  browser.kill('SIGKILL');
  await closed;
  server.kill('SIGTERM');
  await sleep(500);
  if (!process.env.KEEP) {
    try { fs.rmSync(dir, { recursive: true, force: true, maxRetries: 20, retryDelay: 200 }); } catch { /* leftovers are not a failure */ }
  }
  process.exit(code);
};
// whatever goes wrong, the browser and the server must not outlive the test (ctest waits for their output pipes)
for (const event of ['unhandledRejection', 'uncaughtException']) process.on(event, e => { console.error(e); finish(1); });

let targets;
for (let i = 0; i < 100; i++) {
  try { targets = await (await fetch(`http://127.0.0.1:${debugPort}/json`)).json(); if (targets.some(t => t.type === 'page')) break; } catch { /* not up yet */ }
  await sleep(100);
}
const cdp = new WebSocket(targets.find(t => t.type === 'page').webSocketDebuggerUrl);
await new Promise(r => (cdp.onopen = r));
let id = 0;
const pending = new Map();
const problems = [];
cdp.onmessage = m => {
  const x = JSON.parse(m.data);
  if (x.id && pending.has(x.id)) { pending.get(x.id)(x); pending.delete(x.id); }
  else if (x.method === 'Runtime.exceptionThrown') problems.push(x.params.exceptionDetails.exception?.description ?? x.params.exceptionDetails.text);
};
const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); cdp.send(JSON.stringify({ id: i, method, params })); });
await send('Page.enable');
await send('Runtime.enable');
const ev = async e => {
  const r = await send('Runtime.evaluate', { expression: e, returnByValue: true, awaitPromise: true });
  if (r.result?.exceptionDetails) throw new Error(`in page: ${r.result.exceptionDetails.exception?.description ?? r.result.exceptionDetails.text}`);
  return r.result?.result?.value;
};
async function until(what, e, n = 150) {
  for (let i = 0; i < n; i++) { const v = await ev(e); if (v) return v; await sleep(100); }
  throw new Error(`timed out waiting for ${what}; the page says: ${(await ev('document.body.innerText')).slice(0, 300)}; ${problems.join(' | ').slice(0, 300)}`);
}
const shot = async name => fs.writeFileSync(path.join(dir, name), Buffer.from((await send('Page.captureScreenshot', { format: 'png' })).result.data, 'base64'));
/** The center of the first element matching the selector (that has a size), in viewport coordinates. */
const center = async (selector, nth = 0) => ev(`(() => { const e = [...document.querySelectorAll(${JSON.stringify(selector)})].filter(x => x.getBoundingClientRect().width > 0)[${nth}]; if (!e) return null; e.scrollIntoView({ block: 'center' }); const r = e.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`);
/** A point that is really on the element (the middle of a ring, or of an L, is not): searched for on a grid. */
const pointOn = async (selector, nth = 0) => ev(`(() => { const e = [...document.querySelectorAll(${JSON.stringify(selector)})].filter(x => x.getBoundingClientRect().width > 0)[${nth}]; if (!e) return null; e.scrollIntoView({ block: 'center' }); const r = e.getBoundingClientRect(); for (let i = 1; i < 12; i++) for (let j = 1; j < 12; j++) { const x = r.left + r.width * i / 12, y = r.top + r.height * j / 12; if (document.elementFromPoint(x, y) === e) return { x, y }; } return null; })()`);
const move = async (selector, nth = 0) => { const c = (await pointOn(selector, nth)) ?? (await center(selector, nth)); assert(c, `nothing to point at: ${selector}`); await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: c.x, y: c.y }); await sleep(120); return c; };
const press = async (selector, nth = 0) => {
  const c = await move(selector, nth);
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', x: c.x, y: c.y, button: 'left', clickCount: 1 });
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: c.x, y: c.y, button: 'left', clickCount: 1 });
  await sleep(250);
};
const clickText = (selector, text) => ev(`(() => { const e = [...document.querySelectorAll(${JSON.stringify(selector)})].find(x => x.textContent.includes(${JSON.stringify(text)})); if (!e) return false; e.click(); return true; })()`);
const count = selector => ev(`document.querySelectorAll(${JSON.stringify(selector)}).length`);
const tip = () => ev(`(() => { const t = document.querySelector('.tip'); return t && !t.hidden ? t.innerText : ''; })()`);

/** Picks an option of one of the Material dropdowns: opens its panel (again, while the choices are still loading) and clicks the option. */
async function choose(host, text) {
  for (let i = 0; i < 60; i++) {
    await ev(`document.querySelector(${JSON.stringify(host + ' .mat-mdc-select-trigger')})?.click()`);
    await sleep(150);
    const hit = await ev(`(() => { const o = [...document.querySelectorAll('mat-option')].find(x => x.textContent.trim() === ${JSON.stringify(text)}); if (!o) return false; o.click(); return true; })()`);
    if (hit) {
      await sleep(250);
      return;
    }
    await ev(`document.querySelector('.cdk-overlay-backdrop')?.click()`);
    await sleep(200);
  }
  throw new Error(`no option ${text} in ${host}`);
}
async function openReport(title) {
  await until('the list', `[...document.querySelectorAll('.entry .title')].some(e => e.textContent.includes(${JSON.stringify(title)}))`);
  await clickText('.entry', title);
  await until('the form', `!document.querySelector('button.run').disabled`);
  await sleep(400);
  await ev(`document.querySelector('button.run').click()`);
  await until('the page', `document.querySelector('.sheet svg')`);
  await sleep(700); // the bars grow in
}

// ---- charts -----------------------------------------------------------------------------------------------------------------
await send('Page.navigate', { url: `http://127.0.0.1:${port}/reports` });
await openReport('Executive Sales Dashboard');
const bars = await count('.sheet svg .bar');
assert(bars >= 20, `the dashboard has bars (${bars})`);

await move('.sheet svg .slot', 3);
const card = await tip();
assert(/2026-04/.test(card) && /Revenue/.test(card) && /Gross profit/.test(card), `the tooltip should show April's numbers: ${JSON.stringify(card)}`);
assert(/\$?\d[\d,]*/.test(card), 'the tooltip has numbers');
assert(await ev(`document.querySelector('.sheet svg').classList.contains('focus')`), 'the chart is in focus');
assert((await count('.sheet svg .bar.hl')) === 2, 'the two bars of April are highlighted');
await shot('hover.png');
await move('.stage .empty, .bar', 0).catch(() => {});
await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: 5, y: 5 });
await sleep(150);
assert(!(await tip()), 'the card goes when the pointer leaves');

// the legend switches a series off, and back
const legend = await count('.sheet svg .legend');
assert(legend >= 2, 'the legend has its entries');
await press('.sheet svg .legend', 1);
assert((await count('.sheet svg .bar')) < bars, 'switching Gross profit off removes its bars');
await until('Reset view', `document.querySelector('.reset-view')`);
await ev(`document.querySelector('.reset-view').click()`);
await sleep(300);
assert((await count('.sheet svg .bar')) === bars, 'Reset view brings the series back');

// a click selects a category; Escape clears it
await press('.sheet svg .slot', 5);
await until('the selection chip', `document.querySelector('.chip.selection')`);
assert((await ev(`document.querySelector('.chip.selection').textContent`)).includes('2026-06'), 'June is selected');
await ev(`document.querySelector('.chip.selection .clear').click()`);
await sleep(200);
assert(!(await count('.chip.selection')), 'the selection clears');

// the next page: a pie slice shows its share and a click on a legend entry removes it
await ev(`document.querySelector('button.next').click()`);
await sleep(600);
await move('.sheet svg .slice', 0);
assert(/Share/.test(await tip()), 'a pie slice shows its share');
const slices = await count('.sheet svg .slice');
await press('.sheet svg .legend', 2); // after the two entries of the stacked bars
assert((await count('.sheet svg .slice')) === slices - 1, 'a slice leaves the pie when its legend entry is clicked');
await ev(`document.querySelector('.reset-view').click()`);

// drill-through: a click on a region of the donut opens that region's report; Back returns
await ev(`document.querySelector('button.first').click()`);
await sleep(500);
await press('.sheet svg .slice', 0);
await until('the region report', `document.querySelector('.runner h1')?.textContent.includes('Region Snapshot') && document.querySelector('.sheet svg')`);
assert((await ev(`document.querySelector('.sheet svg').textContent`)).includes('Regional snapshot'), 'the region snapshot runs with the clicked region');
assert((await count('.trail')) === 1, 'the trail shows the way back');
await ev(`document.querySelector('.trail .back').click()`);
await until('the dashboard again', `document.querySelector('.runner h1')?.textContent.includes('Executive Sales Dashboard') && document.querySelector('.sheet svg')`);
await sleep(400);

// ---- tables ----------------------------------------------------------------------------------------------------------------------
await openReport('Top Customers');
// the customer in the first row of the table: the text after the heading 'Revenue' and the rank
const firstName = () => ev(`(() => { const t = [...document.querySelectorAll('.sheet svg text')].map(x => x.textContent); const h = t.indexOf('Revenue', t.indexOf('Customer')); return t[h + 2]; })()`);
const before = await firstName();
const th = await ev(`document.querySelectorAll('.sheet svg .th').length`);
assert(th >= 6, `the table has clickable headings (${th})`);
await press('.sheet svg .th', 1); // Customer
const afterAsc = await firstName();
assert(afterAsc !== before, `sorting by Customer changes the first row (${before} -> ${afterAsc})`);
assert(/Click to sort descending/.test(await (async () => { await move('.sheet svg .th', 1); return tip(); })()), 'the heading says what the next click does');
await press('.sheet svg .th', 1);
await press('.sheet svg .th', 1);
assert((await firstName()) === before, 'a third click clears the sort');

await openReport('Revenue by Region and Country');
const rowsOpen = await count('.sheet svg .row');
assert(rowsOpen > 10, 'the table has rows');
await press('.sheet svg .fold', 0);
assert((await count('.sheet svg .row')) < rowsOpen, 'folding a region hides its countries');
await shot('folded.png');
await ev(`document.querySelector('.reset-view').click()`);
await sleep(300);
assert((await count('.sheet svg .row')) === rowsOpen, 'unfolded again');

// the PDF is made of what is shown: switch a series off and export
await openReport('Sales Team Leaderboard');
await press('.sheet svg .legend', 1);
const downloads = path.join(dir, 'dl');
fs.mkdirSync(downloads);
await send('Browser.setDownloadBehavior', { behavior: 'allow', downloadPath: downloads });
await ev(`document.querySelector('button.pdf').click()`);
for (let i = 0; i < 60 && !fs.readdirSync(downloads).some(f => f.endsWith('.pdf')); i++) await sleep(100);
assert(fs.readdirSync(downloads).some(f => f.endsWith('.pdf')), 'the PDF is exported after interacting');

// ---- the designer -------------------------------------------------------------------------------------------------------------------
await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
await until('the tabs', `[...document.querySelectorAll('[role=tab]')].some(t => t.textContent.includes('Report Designer'))`);
await clickText('[role=tab]', 'Report Designer');
await until('the designer', `document.querySelector('app-report-designer app-select.open')`);
await choose('app-report-designer app-select.open', 'Executive Dashboard');
await until('the preview', `document.querySelector('app-report-designer .sheet svg .bar')`);
await sleep(900);
assert((await count('app-data-pane .field')) > 5, 'the data pane lists the fields of the datasets');
assert((await count('app-data-pane .field.number')) > 3 && (await count('app-data-pane .field.text')) > 3, 'with their kinds');

// a click on the chart in the preview opens it in the layout editor
await press('app-report-designer .sheet svg .slot', 0);
await until('the Layout tab', `document.querySelector('app-layout-editor .card.active')`);
const opened = await ev(`document.querySelector('app-layout-editor .card.active').textContent`);
assert(/chart of monthly/i.test(opened), `the chart was opened: ${opened.slice(0, 80)}`);
assert(await ev(`!!document.querySelector('app-report-designer .picked') && !document.querySelector('app-report-designer .picked').hidden`), 'the preview outlines the open item');
assert((await count('app-layout-editor .gallery .type')) === 7, 'the chart type gallery offers seven kinds');
await shot('designer-chart.png');

// choose another chart type: the preview follows, and undo brings the old one back (the trend chart is an area already)
const areas = () => count('app-report-designer .sheet svg path[fill-opacity]');
const areasBefore = await areas();
await ev(`[...document.querySelectorAll('app-layout-editor .gallery .type')].find(b => b.textContent.includes('Areas')).click()`);
await until('more areas', `document.querySelectorAll('app-report-designer .sheet svg path[fill-opacity]').length > ${areasBefore}`);
await ev(`document.querySelector('button.undo').click()`);
await sleep(700);
assert((await areas()) === areasBefore, 'undo takes the area chart away');
await ev(`document.querySelector('button.redo').click()`);
await sleep(700);
assert((await areas()) > areasBefore, 'redo brings it back');

// a field dragged from the data pane onto the Series shelf adds a series (a DragEvent with a DataTransfer, as the browser makes)
const seriesBefore = await count('app-layout-editor .card.active .shelf:nth-of-type(2) .entry');
await ev(`(() => {
  const section = [...document.querySelectorAll('app-data-pane .dataset')].find(d => d.querySelector('.name').textContent.trim() === 'monthly');
  const field = [...section.querySelectorAll('.field')].find(f => f.querySelector('span').textContent.trim() === 'revenue');
  const shelf = [...document.querySelectorAll('app-layout-editor .card.active .shelf')].find(s => s.textContent.includes('Series'));
  const dt = new DataTransfer();
  field.dispatchEvent(new DragEvent('dragstart', { dataTransfer: dt, bubbles: true }));
  shelf.dispatchEvent(new DragEvent('dragover', { dataTransfer: dt, bubbles: true, cancelable: true }));
  shelf.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true }));
})()`);
await sleep(500);
const seriesAfter = await ev(`[...document.querySelectorAll('app-layout-editor .card.active .shelf')].find(s => s.textContent.includes('Series')).querySelectorAll('.entry').length`);
assert(seriesAfter === 3, `dropping a field on the Series shelf adds a series (${seriesAfter})`);
await shot('designer-drop.png');
// a field of another dataset is refused, and says why
await ev(`(() => {
  const section = [...document.querySelectorAll('app-data-pane .dataset')].find(d => d.querySelector('.name').textContent.trim() === 'kpi');
  const field = [...section.querySelectorAll('.field')][0];
  const shelf = [...document.querySelectorAll('app-layout-editor .card.active .shelf')].find(s => s.textContent.includes('Series'));
  const dt = new DataTransfer();
  field.dispatchEvent(new DragEvent('dragstart', { dataTransfer: dt, bubbles: true }));
  shelf.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true }));
})()`);
await sleep(300);
assert(/belongs to kpi/.test(await ev(`document.querySelector('app-layout-editor .notice')?.textContent ?? ''`)), 'a field of another dataset is refused, with the reason');
assert((await ev(`[...document.querySelectorAll('app-layout-editor .card.active .shelf')].find(s => s.textContent.includes('Series')).querySelectorAll('.entry').length`)) === 3, 'and nothing was added');
// a text field is not a series
await ev(`(() => {
  const section = [...document.querySelectorAll('app-data-pane .dataset')].find(d => d.querySelector('.name').textContent.trim() === 'monthly');
  const field = [...section.querySelectorAll('.field:not(.number)')][0]; // the month: a date
  const shelf = [...document.querySelectorAll('app-layout-editor .card.active .shelf')].find(s => s.textContent.includes('Series'));
  const dt = new DataTransfer();
  field.dispatchEvent(new DragEvent('dragstart', { dataTransfer: dt, bubbles: true }));
  shelf.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true }));
})()`);
await sleep(300);
assert(/A series is a number/.test(await ev(`document.querySelector('app-layout-editor .notice')?.textContent ?? ''`)), 'a text field is refused on the Series shelf, with the reason');
assert(problems.length === 0, `errors in the page: ${problems.join(' | ')}`);
void seriesBefore;
console.log('interactive: tooltips, highlight, selection, legend, sort, fold, PDF, click-to-edit, gallery, undo, drag and drop: ok');
await finish(0);
