// The report features in a real browser: starts `sql <db> --listen --reports DIR`, opens the page in headless Chrome (driven
// over the DevTools protocol, no extra packages) and checks that a customer can run a report, see it, export it as PDF and
// CSV, that a report cannot write, that the designer opens and saves a report, and that /reports is the runner alone and
// asks for the token when the server has one.
//   node scripts/reports-e2e.mjs path/to/sql path/to/chrome
import { spawn } from 'node:child_process';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const binary = process.argv[2];
const chrome = process.argv[3] ?? process.env.CHROME_BIN;
assert(binary && chrome, 'usage: node scripts/reports-e2e.mjs path/to/sql path/to/chrome');

const sleep = ms => new Promise(r => setTimeout(r, ms));
const freePort = () =>
  new Promise(resolve => {
    const probe = net.createServer().listen(0, '127.0.0.1', () => {
      const { port } = probe.address();
      probe.close(() => resolve(port));
    });
  });
const run = (...args) => new Promise((resolve, reject) => spawn(binary, args, { stdio: 'ignore' }).on('exit', c => (c === 0 ? resolve() : reject(new Error(`sql ${args.join(' ')} failed`)))));

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'sql-reports-'));
const file = path.join(dir, 'r.sqldb');
const reports = path.join(dir, 'reports');
const downloads = path.join(dir, 'downloads');
fs.mkdirSync(reports);
fs.mkdirSync(downloads);

// ---- data and reports ---------------------------------------------------------------------------------------------------
await run(file, '-c', 'CREATE TABLE sales(region TEXT, month TEXT, amount REAL, units INTEGER)');
const rows = [];
for (const [r, region] of ['East', 'North', 'South', 'West'].entries())
  for (let m = 1; m <= 12; m++) rows.push(`('${region}', '2026-${String(m).padStart(2, '0')}', ${1000 + r * 500 + m * 37}, ${m + r})`);
await run(file, '-c', `INSERT INTO sales VALUES ${rows.join(',')}`);

const sales = {
  format: 1,
  name: 'Sales',
  title: 'Sales by region — Продажи',
  description: 'Amounts and units per month',
  page: { size: 'A4', orientation: 'portrait', margins: { top: 48, right: 40, bottom: 48, left: 40 } },
  parameters: [{ name: 'region', label: 'Region', type: 'text', optionsSql: 'SELECT DISTINCT region FROM sales ORDER BY 1' }],
  datasets: [{ name: 'sales', sql: 'SELECT region, month, amount, units FROM sales WHERE (:region IS NULL OR region = :region) ORDER BY region, month' }],
  header: [{ type: 'text', text: '{{ReportName}}', style: { face: 'bold', size: 10, color: '#555555' } }, { type: 'rule' }],
  footer: [{ type: 'rule' }, { type: 'text', text: 'Page {{PageNumber}} of {{PageCount}}', style: { size: 9, align: 'right' } }],
  body: [
    { type: 'text', text: '{{ReportName}}', style: { face: 'bold', size: 20 } },
    { type: 'text', text: 'Total {{Format(Sum(amount), "#,##0")}} in {{Count()}} rows', dataset: 'sales' },
    { type: 'chart', chart: 'bar', dataset: 'sales', category: 'month', series: [{ field: 'amount', label: 'Amount' }], title: 'Amount per month', height: 180 },
    {
      type: 'table', dataset: 'sales', zebra: true, grandTotal: true,
      groups: [{ field: 'region', header: 'Region {{region}}', subtotals: true }],
      columns: [{ field: 'month', header: 'Month' }, { field: 'amount', header: 'Amount', format: '#,##0.00', total: 'sum' }, { field: 'units', header: 'Units', total: 'sum' }],
    },
  ],
};
const bad = { ...sales, name: 'Bad', title: 'Tries to write', parameters: [], datasets: [{ name: 'sales', sql: 'DELETE FROM sales' }], body: [{ type: 'text', text: 'never shown' }] };
fs.writeFileSync(path.join(reports, 'Sales.json'), JSON.stringify(sales));
fs.writeFileSync(path.join(reports, 'Bad.json'), JSON.stringify(bad));

// ---- servers ------------------------------------------------------------------------------------------------------------
const servers = [];
async function serve(extra = []) {
  const port = await freePort();
  const server = spawn(binary, [file, '--listen', String(port), '--reports', reports, ...extra], { stdio: ['ignore', 'pipe', 'inherit'] });
  servers.push(server);
  await new Promise((resolve, reject) => {
    server.stdout.once('data', resolve);
    server.on('exit', code => reject(new Error(`the server exited with ${code}`)));
  });
  return port;
}
const port = await serve();

// ---- the browser --------------------------------------------------------------------------------------------------------
const debugPort = await freePort();
const browser = spawn(chrome, [`--remote-debugging-port=${debugPort}`, '--headless=new', '--no-sandbox', '--disable-gpu', `--user-data-dir=${dir}/profile`, '--window-size=1400,1000', 'about:blank'], { stdio: 'ignore' });
const finish = async code => {
  const closed = new Promise(resolve => browser.once('exit', resolve));
  browser.kill('SIGKILL');
  await closed;
  for (const s of servers) s.kill('SIGTERM');
  await sleep(500); // Chrome's helper processes may still be writing to the profile
  if (!process.env.KEEP) {
    try {
      fs.rmSync(dir, { recursive: true, force: true, maxRetries: 20, retryDelay: 200 });
    } catch {
      /* a leftover temporary directory is not a failure of what was tested */
    }
  }
  process.exit(code);
};
// whatever goes wrong, the browser and the server must not outlive the test (ctest waits for their output pipes)
for (const event of ['unhandledRejection', 'uncaughtException']) process.on(event, e => { console.error(e); finish(1); });

let targets;
for (let i = 0; i < 100; i++) {
  try {
    targets = await (await fetch(`http://127.0.0.1:${debugPort}/json`)).json();
    if (targets.some(t => t.type === 'page')) break;
  } catch {
    /* not up yet */
  }
  await sleep(100);
}
const cdp = new WebSocket(targets.find(t => t.type === 'page').webSocketDebuggerUrl);
await new Promise(resolve => (cdp.onopen = resolve));
let nextId = 1;
const pending = new Map();
const problems = [];
cdp.onmessage = m => {
  const msg = JSON.parse(m.data);
  if (msg.id && pending.has(msg.id)) {
    pending.get(msg.id)(msg);
    pending.delete(msg.id);
  } else if (msg.method === 'Runtime.exceptionThrown') problems.push(msg.params.exceptionDetails.exception?.description ?? msg.params.exceptionDetails.text);
  else if (msg.method === 'Runtime.consoleAPICalled' && msg.params.type === 'error') problems.push(msg.params.args.map(a => a.description ?? a.value).join(' '));
};
const send = (method, params = {}) => new Promise(resolve => { const id = nextId++; pending.set(id, resolve); cdp.send(JSON.stringify({ id, method, params })); });
await send('Page.enable');
await send('Runtime.enable');
await send('Browser.setDownloadBehavior', { behavior: 'allow', downloadPath: downloads });
const evaluate = async expression => {
  const r = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
  if (r.result?.exceptionDetails) throw new Error(`in page: ${r.result.exceptionDetails.exception?.description ?? r.result.exceptionDetails.text}`);
  return r.result?.result?.value;
};
async function until(what, expression, tries = 150) {
  for (let i = 0; i < tries; i++) {
    const v = await evaluate(expression);
    if (v) return v;
    await sleep(100);
  }
  throw new Error(`timed out waiting for ${what}; the page says: ${(await evaluate('document.body.innerText')).slice(0, 400)}; the console: ${problems.slice(0, 3).join(' | ').slice(0, 600)}`);
}
const click = (selector, text = '') =>
  evaluate(`(() => { const e = [...document.querySelectorAll(${JSON.stringify(selector)})].find(x => x.textContent.includes(${JSON.stringify(text)})); if (!e) return false; e.click(); return true; })()`);
const shot = async name => fs.writeFileSync(path.join(dir, name), Buffer.from((await send('Page.captureScreenshot', { format: 'png' })).result.data, 'base64'));
const setValue = (selector, value, event = 'input') =>
  evaluate(`(() => { const e = document.querySelector(${JSON.stringify(selector)}); e.value = ${JSON.stringify(value)}; e.dispatchEvent(new Event(${JSON.stringify(event)}, { bubbles: true })); return true; })()`);
/** Picks an option of one of the Material dropdowns: opens its panel (again, while the choices are still loading) and clicks the option. */
async function choose(host, text) {
  for (let i = 0; i < 60; i++) {
    await evaluate(`document.querySelector(${JSON.stringify(host + ' .mat-mdc-select-trigger')})?.click()`);
    await sleep(150);
    const hit = await evaluate(`(() => { const o = [...document.querySelectorAll('mat-option')].find(x => x.textContent.trim() === ${JSON.stringify(text)}); if (!o) return false; o.click(); return true; })()`);
    if (hit) {
      await sleep(250);
      return;
    }
    await evaluate(`document.querySelector('.cdk-overlay-backdrop')?.click()`);
    await sleep(200);
  }
  throw new Error(`no option ${text} in ${host}`);
}
const waitFile = async (name, tries = 100) => {
  const p = path.join(downloads, name);
  for (let i = 0; i < tries; i++) {
    if (fs.existsSync(p) && !fs.existsSync(p + '.crdownload')) {
      await sleep(100);
      return fs.readFileSync(p);
    }
    await sleep(100);
  }
  throw new Error(`${name} was not downloaded; got ${fs.readdirSync(downloads)}`);
};

// ---- a customer runs a report ----------------------------------------------------------------------------------------------
await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
await until('the tabs', `[...document.querySelectorAll('[role=tab]')].some(t => t.textContent.includes('Reports'))`);
assert(await click('[role=tab]', 'Reports'), 'no Reports tab');
await until('the report list', `[...document.querySelectorAll('.entry')].some(e => e.textContent.includes('Sales by region'))`);
await click('.entry', 'Sales by region');
await choose('app-report-runner app-select', 'North');
await until('Run enabled', `!document.querySelector('button.run').disabled`);
await click('button.run');
await until('the first page', `document.querySelector('.sheet svg')`);
let pageText = await evaluate(`document.querySelector('.sheet svg').textContent`);
assert(pageText.includes('Sales by region'), `the title is missing from the page: ${pageText.slice(0, 200)}`);
assert(pageText.includes('Total 20,886 in 12 rows'), `the dataset aggregate is wrong: ${pageText.slice(0, 300)}`);
assert(pageText.includes('Region North'), 'the group heading is missing');
assert(!pageText.includes('Region East'), 'the region parameter was not applied');
const pagesInfo = await evaluate(`document.querySelector('.viewer .number').value + ' ' + document.querySelector('.viewer .page').textContent.replace(/\\s+/g, ' ')`);
assert(/^1 .*of [1-9]\d*/.test(pagesInfo), pagesInfo);
await shot('runner.png');

await click('button.pdf');
const pdf = await waitFile('Sales.pdf');
assert.equal(pdf.subarray(0, 5).toString(), '%PDF-');
assert(pdf.length > 8000 && pdf.length < 200000, `odd PDF size ${pdf.length}`);
assert(pdf.toString('latin1').includes('/Subtype /CIDFontType2'), 'no embedded font');
await click('button.csv');
const csv = (await waitFile('Sales.csv')).toString('utf8');
assert(csv.startsWith('﻿Month,Amount,Units\r\n'), csv.slice(0, 60));
assert.equal(csv.trim().split('\r\n').length, 13, 'the CSV should hold the 12 rows of North and the heading');
assert(csv.includes('2026-03,'), 'a month is missing from the CSV');

// ---- a report cannot write -------------------------------------------------------------------------------------------------
await click('.entry', 'Tries to write');
await until('Run enabled', `!document.querySelector('button.run').disabled`);
await click('button.run');
const failure = await until('the error', `document.querySelector('.error')?.textContent`);
assert(/read-only|read only|cannot|ReadOnly/i.test(failure), `a write should be refused: ${failure}`);
// the data is all there: the Sales report over every region still counts 48 rows
await click('.entry', 'Sales by region');
await until('Run enabled', `!document.querySelector('button.run').disabled`);
await click('button.run');
await until('all rows', `document.querySelector('.sheet svg')?.textContent.includes('in 48 rows')`);

// ---- the designer ----------------------------------------------------------------------------------------------------------
assert(await click('[role=tab]', 'Report Designer'), 'no Report Designer tab');
await until('the designer', `document.querySelector('app-report-designer app-select.open')`);
await choose('app-report-designer app-select.open', 'Sales');
await until('the preview', `document.querySelector('app-report-designer .sheet svg')`);
await click('[role=tab]', 'Layout');
await until('the layout items', `document.querySelector('app-layout-editor .card')`);
assert((await evaluate(`document.querySelector('app-layout-editor').textContent`)).includes('Table of sales'), 'the table item is not listed');
await click('[role=tab]', 'Datasets');
await until('the dataset editor', `document.querySelector('app-datasets-editor app-sql-editor')`);
await click('app-datasets-editor button.run');
await until('the dataset sample', `document.querySelector('app-datasets-editor .sample tbody tr')`);
await click('[role=tab]', 'JSON');
await until('the JSON', `document.querySelector('.json textarea')?.value.includes('"Sales"')`);
await until('the JSON colored', `document.querySelector('.json .back .key') && document.querySelector('.json .back .str')`);
await shot('json.png');
const edited = JSON.stringify({ ...sales, title: 'Edited in the designer' }, null, 2);
await setValue('.json textarea', edited);
await until('the change to show as unsaved', `document.querySelector('.dirty')`);
await click('button.save');
for (let i = 0; i < 50 && !fs.readFileSync(path.join(reports, 'Sales.json'), 'utf8').includes('Edited in the designer'); i++) await sleep(100);
assert(fs.readFileSync(path.join(reports, 'Sales.json'), 'utf8').includes('Edited in the designer'), 'the designer did not save the report');
await shot('designer.png');
// the theme button: dark for everything but the page, which is paper
const bg = () => evaluate(`getComputedStyle(document.body).backgroundColor`);
const lightBg = await bg();
await click('app-theme-toggle button.theme');
await until('the theme menu', `document.querySelector('.theme-dark')`);
await click('.theme-dark');
await until('the dark theme', `document.documentElement.dataset.theme === 'dark'`);
assert((await bg()) !== lightBg, 'the dark theme did not change the background');
assert((await evaluate(`localStorage.getItem('sql.theme')`)) === 'dark', 'the theme was not remembered');
await click('[role=tab]', 'Layout');
assert((await evaluate(`getComputedStyle(document.querySelector('app-report-designer .sheet')).backgroundColor`)) === 'rgb(255, 255, 255)', 'the page is not white in the dark theme');
await shot('designer-dark.png');
await click('app-theme-toggle button.theme');
await until('the theme menu', `document.querySelector('.theme-light')`);
await click('.theme-light');
await until('the light theme', `document.documentElement.dataset.theme === 'light'`);

// ---- /reports is the runner alone ----------------------------------------------------------------------------------------------
await send('Page.navigate', { url: `http://127.0.0.1:${port}/reports` });
await until('the runner page', `document.querySelector('app-reports-page .entry')`);
const only = await evaluate('document.body.innerText');
assert(only.includes('Edited in the designer'), 'the saved title is not listed');
assert(!only.includes('Database Structure') && !only.includes('Execute SQL'), 'the customer page shows database tools');

// ---- a server with a token --------------------------------------------------------------------------------------------------
// the store allows one process: the first server makes way
const first = servers[0];
const gone = new Promise(resolve => first.once('exit', resolve));
first.kill('SIGTERM');
await gone;
const guarded = await serve(['--token', 'sesame']);
await send('Page.navigate', { url: `http://127.0.0.1:${guarded}/reports` });
await until('the token form', `document.querySelector('form.token input')`);
assert(!(await evaluate(`!!document.querySelector('app-reports-page .entry')`)), 'reports are listed without the token');
await setValue('form.token input', 'sesame');
await click('form.token button');
await until('the reports after the token', `document.querySelector('app-reports-page .entry')`);
await shot('guarded.png');

console.log('reports: run, view, PDF, CSV, read-only, designer save, runner page and token: ok');
await finish(0);
