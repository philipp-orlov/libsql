// The browser's SqlSocket (src/app/remote) against a running `sql <db> --listen PORT`, from Node (which has a
// WebSocket of its own): connects, runs queries, reads the rows, streams a large result, interrupts a long
// statement and checks the token. Exits non-zero on the first thing that differs from what is expected.
//   node scripts/socket-smoke.mjs [port] [token]
import { build } from 'esbuild';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const port = process.argv[2] ?? '8080';
const token = process.argv[3];
const out = await build({ entryPoints: [path.join(root, 'src/app/remote/index.ts')], bundle: true, write: false, format: 'esm', platform: 'node' });
const { SqlSocket, SocketError } = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));

const url = `ws://127.0.0.1:${port}/api/v1/ws/sql`;
const rows = view => Array.from({ length: view.rowCount }, (_, r) => Array.from({ length: view.columns.length }, (_, c) => view.cell(r, c)));

if (token) {
  const anonymous = new SqlSocket(url, { pingMs: 0 });
  await assert.rejects(anonymous.connect(), e => e instanceof SocketError);
  console.log('refused without the token: ok');
}
const socket = new SqlSocket(url, { token, pingMs: 0 });
await socket.connect();

let r = await socket.exec('SELECT 1 AS one, 2.5 AS two, \'three\' AS three, NULL AS four');
assert.deepEqual(r.view.columns, ['one', 'two', 'three', 'four']);
assert.deepEqual(rows(r.view), [[1, 2.5, 'three', null]]);
console.log('a query: ok');

await assert.rejects(socket.exec('SELECT * FROM no_such_table'), e => e instanceof SocketError && /no such table/.test(e.message));
console.log('an error comes back as an error: ok');

await socket.exec('CREATE TABLE smoke(n INTEGER, s TEXT)');
r = await socket.exec('SELECT 1');
assert.equal(r.done.inTransaction, true, 'a change stays pending until written');
await socket.exec("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 200000) INSERT INTO smoke SELECT n, 'row ' || n FROM c");
const started = performance.now();
r = await socket.exec('SELECT n, s FROM smoke ORDER BY n');
const took = performance.now() - started;
assert.equal(r.view.rowCount, 200000);
assert.equal(r.view.cell(199999, 0), 200000);
assert.equal(r.view.cell(0, 1), 'row 1');
console.log(`200000 rows streamed in ${r.view.batchCount} batch(es), ${took.toFixed(0)} ms: ok`);

const long = socket.exec('WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 1000000000) SELECT count(*) FROM c');
setTimeout(() => socket.interrupt(), 300);
await assert.rejects(long, e => e instanceof SocketError && e.code === 17);
console.log('interrupt: ok');

r = await socket.exec('SELECT count(*) FROM smoke');
assert.equal(r.view.cell(0, 0), 200000);
await socket.exec('ROLLBACK');
socket.close();
console.log('all ok');
process.exit(0);
