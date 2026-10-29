// The real page in a real browser: starts `sql <db> --listen`, opens its page in headless Chrome (driven over the
// DevTools protocol, no extra packages) and checks that the page talks to the server with librpc -- the WebSocket
// is opened with sub-protocol librpc.v1 and the database's tables show up on the page, so the front end's RPC
// client works in a browser, not just under Node.
//   node scripts/browser-e2e.mjs path/to/sql [path/to/chrome]    (or CHROME_BIN)
import { spawn } from 'node:child_process';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const binary = process.argv[2];
const chrome = process.argv[3] ?? process.env.CHROME_BIN;
assert(binary && chrome, 'usage: node scripts/browser-e2e.mjs path/to/sql path/to/chrome');

const freePort = () =>
  new Promise(resolve => {
    const probe = net.createServer().listen(0, '127.0.0.1', () => {
      const { port } = probe.address();
      probe.close(() => resolve(port));
    });
  });
const sleep = ms => new Promise(r => setTimeout(r, ms));

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'sql-browser-'));
const file = path.join(dir, 'b.sqldb');
for (const statement of ["CREATE TABLE zebra_stripes(id INTEGER PRIMARY KEY, name TEXT)", "INSERT INTO zebra_stripes VALUES (1, 'a'), (2, 'b'), (3, 'c')"]) {
  await new Promise((resolve, reject) => spawn(binary, [file, '-c', statement], { stdio: 'ignore' }).on('exit', c => (c === 0 ? resolve() : reject(new Error('seeding failed')))));
}
const port = await freePort();
const server = spawn(binary, [file, '--listen', String(port)], { stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise((resolve, reject) => {
  server.stdout.once('data', resolve);
  server.on('exit', code => reject(new Error(`the server exited with ${code}`)));
});
const debugPort = await freePort();
const browser = spawn(chrome, [`--remote-debugging-port=${debugPort}`, '--no-sandbox', '--disable-gpu', `--user-data-dir=${dir}/profile`, 'about:blank'], { stdio: 'ignore' });
const finish = async code => {
  const closed = new Promise(resolve => browser.once('exit', resolve));
  browser.kill('SIGKILL');
  await closed;
  server.kill('SIGTERM');
  fs.rmSync(dir, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
  process.exit(code);
};
process.on('unhandledRejection', e => {
  console.error(e);
  finish(1);
});

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
const page = targets.find(t => t.type === 'page');
assert(page, 'no browser page');
const cdp = new WebSocket(page.webSocketDebuggerUrl);
await new Promise(resolve => (cdp.onopen = resolve));
let nextId = 1;
const pending = new Map();
const events = [];
cdp.onmessage = m => {
  const msg = JSON.parse(m.data);
  if (msg.id && pending.has(msg.id)) {
    pending.get(msg.id)(msg);
    pending.delete(msg.id);
  } else if (msg.method) {
    events.push(msg);
  }
};
const send = (method, params = {}) =>
  new Promise(resolve => {
    const id = nextId++;
    pending.set(id, resolve);
    cdp.send(JSON.stringify({ id, method, params }));
  });
await send('Network.enable');
await send('Page.enable');
await send('Runtime.enable');
await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });

const evaluate = async expression => (await send('Runtime.evaluate', { expression, returnByValue: true })).result?.result?.value;
let text = '';
for (let i = 0; i < 200 && !text.includes('zebra_stripes'); i++) {
  await sleep(100);
  text = (await evaluate('document.body ? document.body.innerText : ""')) ?? '';
}
assert(text.includes('zebra_stripes'), `the page never showed the table; it shows: ${text.slice(0, 300)}`);

const created = events.find(e => e.method === 'Network.webSocketCreated');
assert(created && created.params.url.endsWith('/api/v1/ws/sql'), 'the page did not open the SQL WebSocket');
const handshake = events.find(e => e.method === 'Network.webSocketHandshakeResponseReceived');
assert(handshake, 'no WebSocket handshake');
const headers = Object.fromEntries(Object.entries(handshake.params.response.headers).map(([k, v]) => [k.toLowerCase(), v]));
assert.equal(headers['sec-websocket-protocol'], 'librpc.v1', 'the page did not speak librpc.v1');
assert.equal(handshake.params.response.status, 101);
const frames = events.filter(e => e.method === 'Network.webSocketFrameReceived').length;
assert(frames > 2, 'no frames came back');
console.log(`the page opened ${created.params.url} with sub-protocol librpc.v1, exchanged frames, and shows the tables: ok`);
await finish(0);
