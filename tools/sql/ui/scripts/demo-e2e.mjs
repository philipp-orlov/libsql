// The demo (libsql/demo) in a real browser: copies the demo database and its reports, serves them with `sql --listen`, and
// checks that every report runs on the customer page (/reports) without an error, draws at least a page, and exports a PDF
// that opens (starts with %PDF, embeds its font) and a CSV.
//   node scripts/demo-e2e.mjs path/to/sql path/to/chrome path/to/demo
import { spawn } from 'node:child_process';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const [binary, chrome, demo] = process.argv.slice(2);
assert(binary && chrome && demo, 'usage: node scripts/demo-e2e.mjs path/to/sql path/to/chrome path/to/demo');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const freePort = () => new Promise(res => { const p = net.createServer().listen(0, '127.0.0.1', () => { const { port } = p.address(); p.close(() => res(port)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'sql-demo-'));
const downloads = path.join(dir, 'downloads');
fs.mkdirSync(downloads);
fs.copyFileSync(path.join(demo, 'sales_demo.sqldb'), path.join(dir, 'sales_demo.sqldb'));
fs.cpSync(path.join(demo, 'sales_demo.sqldb.reports'), path.join(dir, 'sales_demo.sqldb.reports'), { recursive: true });
const expected = fs.readdirSync(path.join(dir, 'sales_demo.sqldb.reports')).filter(f => f.endsWith('.json'));
assert(expected.length >= 5, 'the demo should come with several reports');

const port = await freePort();
const server = spawn(binary, [path.join(dir, 'sales_demo.sqldb'), '--listen', String(port)], { stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise((resolve, reject) => {
  server.stdout.once('data', resolve);
  server.on('exit', code => reject(new Error(`the server exited with ${code}`)));
});
const debugPort = await freePort();
const browser = spawn(chrome, [`--remote-debugging-port=${debugPort}`, '--headless=new', '--no-sandbox', '--disable-gpu', `--user-data-dir=${dir}/profile`, '--window-size=1500,1000', 'about:blank'], { stdio: 'ignore' });
const finish = async code => {
  const closed = new Promise(resolve => browser.once('exit', resolve));
  browser.kill('SIGKILL');
  await closed;
  server.kill('SIGTERM');
  await sleep(500); // Chrome's helper processes may still be writing to the profile
  try {
    fs.rmSync(dir, { recursive: true, force: true, maxRetries: 20, retryDelay: 200 });
  } catch {
    /* a leftover temporary directory is not a failure of what was tested */
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
await send('Browser.setDownloadBehavior', { behavior: 'allow', downloadPath: downloads });
const ev = async e => (await send('Runtime.evaluate', { expression: e, returnByValue: true, awaitPromise: true })).result?.result?.value;
async function until(what, e, n = 200) {
  for (let i = 0; i < n; i++) { const v = await ev(e); if (v) return v; await sleep(100); }
  throw new Error(`timed out waiting for ${what}; the page says: ${(await ev('document.body.innerText')).slice(0, 300)}; ${problems.join(' | ').slice(0, 300)}`);
}

await send('Page.navigate', { url: `http://127.0.0.1:${port}/reports` });
await until('the report list', `document.querySelectorAll('.entry').length >= ${expected.length}`);
const titles = await ev(`[...document.querySelectorAll('.entry .title')].map(e => e.textContent)`);
const results = [];
for (const [i, title] of titles.entries()) {
  await ev(`[...document.querySelectorAll('.entry')][${i}].click()`);
  await until(`${title}: the form`, `!document.querySelector('button.run').disabled`);
  await sleep(300); // the choices of a parameter load
  await ev(`document.querySelector('button.run').click()`);
  await until(`${title}: a page or an error`, `document.querySelector('.sheet svg') || document.querySelector('.error')`, 300);
  const error = await ev(`document.querySelector('.error')?.textContent`);
  assert(!error, `${title}: ${error}`);
  const text = await ev(`document.querySelector('.sheet svg').textContent`);
  assert(!text.includes('#Error'), `${title}: an expression failed: ${text.slice(0, 200)}`);
  const warnings = await ev(`[...document.querySelectorAll('.warning')].map(e => e.textContent).join('; ')`);
  assert(!warnings, `${title}: ${warnings}`);
  const pages = Number(await ev(`document.querySelector('.viewer .page').textContent.match(/of\\s+(\\d+)/)[1]`));
  assert(pages >= 1, `${title}: no pages`);
  const file = `${title.replace(/[^A-Za-z0-9]+/g, '_')}.pdf`;
  await ev(`document.querySelector('button.pdf').click()`);
  let pdf;
  for (let k = 0; k < 100 && !pdf; k++) {
    const found = fs.readdirSync(downloads).filter(f => f.endsWith('.pdf') && !results.some(r => r.pdf === f));
    if (found.length) { await sleep(150); pdf = found[0]; } else await sleep(100);
  }
  assert(pdf, `${title}: no PDF`);
  const bytes = fs.readFileSync(path.join(downloads, pdf));
  assert.equal(bytes.subarray(0, 5).toString(), '%PDF-');
  assert(bytes.toString('latin1').includes('/CIDFontType2'), `${title}: no embedded font`);
  results.push({ title, pages, pdf, size: bytes.length });
}
assert.equal(results.length, expected.length);
assert(problems.length === 0, `errors in the page: ${problems.join(' | ')}`);
console.log(results.map(r => `${r.title}: ${r.pages} page${r.pages === 1 ? '' : 's'}, PDF ${Math.round(r.size / 1024)} KiB`).join('\n'));
console.log(`demo: ${results.length} reports run and export as PDF: ok`);
await finish(0);
