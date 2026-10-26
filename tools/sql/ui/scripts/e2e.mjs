// The front end's data layer (src/app/services/catalog.ts over src/app/remote) against a real `sql <db> --listen`:
// starts the binary itself, then runs what every tab does, schema, browse with sort and filters, edits that stay
// pending until written, pragmas and the dump, with only the SQL WebSocket route between them.
//   node scripts/e2e.mjs path/to/sql
import { build } from 'esbuild';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const binary = process.argv[2];
assert(binary, 'usage: node scripts/e2e.mjs path/to/sql');

const bundle = await build({
  stdin: {
    contents: "export * from './src/app/remote/index.ts'; export * as catalog from './src/app/services/catalog.ts';",
    resolveDir: root,
    loader: 'ts',
  },
  bundle: true,
  write: false,
  format: 'esm',
  platform: 'node',
});
const { SqlSocket, SocketError, ErrorCode, Catalog, catalog } = await import('data:text/javascript;base64,' + Buffer.from(bundle.outputFiles[0].text).toString('base64'));

const freePort = () =>
  new Promise(resolve => {
    const probe = net.createServer().listen(0, '127.0.0.1', () => {
      const { port } = probe.address();
      probe.close(() => resolve(port));
    });
  });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'sql-e2e-'));
const file = path.join(dir, 'e2e.sqldb');
const port = await freePort();
const token = 's3cret';
const server = spawn(binary, [file, '--listen', String(port), '--token', token], { stdio: ['ignore', 'pipe', 'inherit'] });
let exited = null;
server.on('exit', code => (exited = code));
await new Promise((resolve, reject) => {
  server.stdout.once('data', resolve);
  server.on('exit', code => reject(new Error(`the server exited with ${code}`)));
});

const finish = code => {
  server.kill('SIGTERM');
  fs.rmSync(dir, { recursive: true, force: true });
  process.exit(code);
};
process.on('uncaughtException', e => {
  console.error(e);
  finish(1);
});
process.on('unhandledRejection', e => {
  console.error(e);
  finish(1);
});

const url = `ws://127.0.0.1:${port}/api/v1/ws/sql`;
const ok = what => console.log(`${what}: ok`);

// A token is needed, and the page presents it in its first message.
await assert.rejects(new SqlSocket(url, { pingMs: 0 }).connect(), e => e instanceof SocketError && e.code === ErrorCode.PermissionDenied);
ok('the route refuses a client without the token');
const socket = new SqlSocket(url, { token, pingMs: 0 });
await socket.connect();
assert.match(socket.welcome.server, /^sql\//);
assert.equal(socket.welcome.readOnly, false);
const conn = {
  exec: (sql, params, options) => socket.exec(sql, params, options),
  describe: (what, name) => socket.describe(what, name),
};
const run = sql => conn.exec(sql);
const rows = async sql => catalog.rowsOf(await conn.exec(sql));

// ---- a schema to work on
await run(`CREATE TABLE person(id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER, note TEXT);
           CREATE TABLE pair(a TEXT, b INTEGER, v TEXT, PRIMARY KEY(a, b)) WITHOUT ROWID;
           CREATE TABLE typed(i INTEGER, r REAL, t TEXT, b BLOB) STRICT;
           CREATE VIEW adults AS SELECT id, name FROM person WHERE age >= 18;
           CREATE INDEX person_name ON person(name, age);
           CREATE TRIGGER person_added AFTER INSERT ON person BEGIN SELECT 1; END;`);
await run("INSERT INTO person(name, age, note) VALUES ('Ada', 36, 'first'), ('Bob', 17, NULL), ('Cyd', 52, 'third')");
await run('COMMIT');
ok('a schema, written');

// ---- facts and schema
const facts = await catalog.loadFacts(conn);
assert.equal(fs.realpathSync(facts.path), fs.realpathSync(file));
assert(facts.functions.includes('count') && facts.functions.includes('lower'));
ok('path and function names');

const schema = await catalog.loadSchema(conn);
assert.deepEqual(schema.tables.map(t => t.name).sort(), ['pair', 'person', 'typed']);
const person = schema.tables.find(t => t.name === 'person');
assert.deepEqual(person.columns.map(c => [c.name, c.type, c.primaryKey, c.notNull]), [
  ['id', 'INTEGER', true, false],
  ['name', 'TEXT', false, true],
  ['age', 'INTEGER', false, false],
  ['note', 'TEXT', false, false],
]);
assert.equal(person.columns[0].affinity, 'INTEGER');
assert.match(person.sql, /CREATE TABLE person/);
assert.equal(schema.tables.find(t => t.name === 'pair').withoutRowid, true);
assert.equal(schema.tables.find(t => t.name === 'typed').strict, true);
assert.deepEqual(schema.views.map(v => [v.name, v.columns.map(c => c.name)]), [['adults', ['id', 'name']]]);
assert.deepEqual(schema.indexes.map(i => [i.name, i.table, i.columns]), [['person_name', 'person', ['name', 'age']]]);
assert.deepEqual(schema.triggers.map(t => [t.name, t.table]), [['person_added', 'person']]);
ok('the schema');

// ---- browse
let page = await catalog.browse(conn, { table: 'person', offset: 0, limit: 100, sort: [], filters: {} }, false);
assert.equal(page.total, 3);
assert.deepEqual(page.rows, [[1, 'Ada', 36, 'first'], [2, 'Bob', 17, null], [3, 'Cyd', 52, 'third']]);
assert.deepEqual(page.keys, [[1], [2], [3]]);
assert.equal(page.editable, true);
page = await catalog.browse(conn, { table: 'PERSON', offset: 1, limit: 1, sort: [{ column: 'age', descending: true }], filters: {} }, false);
assert.deepEqual(page.rows, [[1, 'Ada', 36, 'first']]);
assert.equal(page.total, 3);
page = await catalog.browse(conn, { table: 'person', offset: 0, limit: 10, sort: [], filters: { age: '>=18', name: 'y' } }, false);
assert.deepEqual(page.rows.map(r => r[1]), ['Cyd']);
page = await catalog.browse(conn, { table: 'person', offset: 0, limit: 10, sort: [], filters: { note: '=null' } }, false);
assert.deepEqual(page.rows.map(r => r[1]), ['Bob']);
page = await catalog.browse(conn, { table: 'adults', offset: 0, limit: 10, sort: [], filters: {} }, false);
assert.equal(page.view, true);
assert.equal(page.editable, false);
assert.equal(page.total, 2);
await assert.rejects(catalog.browse(conn, { table: 'missing', offset: 0, limit: 1, sort: [], filters: {} }, false), /no such table/);
ok('browse: pages, sort, filters, views');

// ---- edits stay pending until written
assert.equal((await run('SELECT 1')).done.inTransaction, false);
await catalog.insertRow(conn, 'person', { name: 'Dee', age: 29 }, false);
assert.equal((await run('SELECT 1')).done.inTransaction, true, 'an edit opens a pending transaction');
await catalog.updateRow(conn, 'person', [1], { note: 'edited', age: { type: 'integer', value: '37' } }, false);
await catalog.deleteRows(conn, 'person', [[2]], false);
page = await catalog.browse(conn, { table: 'person', offset: 0, limit: 100, sort: [], filters: {} }, false);
assert.deepEqual(page.rows.map(r => r.slice(1)), [['Ada', 37, 'edited'].slice(0, 2).concat(['edited']), ['Cyd', 52, 'third'], ['Dee', 29, null]].map(r => r));
await assert.rejects(catalog.updateRow(conn, 'person', [99], { name: 'x' }, false), /no longer exists/);
await run('ROLLBACK');
page = await catalog.browse(conn, { table: 'person', offset: 0, limit: 100, sort: [], filters: {} }, false);
assert.equal(page.total, 3, 'reverted');
assert.equal((await run('SELECT 1')).done.inTransaction, false);
await catalog.insertRow(conn, 'person', { name: 'Eve' }, false);
await run('COMMIT');
assert.equal((await rows("SELECT count(*) FROM person WHERE name = 'Eve'"))[0][0], 1);
ok('insert, update, delete; revert; write');

// a failed constraint leaves nothing half done
await assert.rejects(catalog.insertRow(conn, 'person', { name: null }, false), e => /NOT NULL|constraint/i.test(e.message));
assert.equal((await run('SELECT 1')).done.inTransaction, false);
ok('a refused edit leaves nothing pending');

// WITHOUT ROWID: rows are addressed by their key
await catalog.insertRow(conn, 'pair', { a: 'x', b: 1, v: 'one' }, false);
await catalog.insertRow(conn, 'pair', { a: 'x', b: 2, v: 'two' }, false);
page = await catalog.browse(conn, { table: 'pair', offset: 0, limit: 10, sort: [], filters: {} }, false);
assert.deepEqual(page.keys, [['x', 1], ['x', 2]]);
await catalog.updateRow(conn, 'pair', ['x', 2], { v: 'deux' }, false);
await catalog.deleteRows(conn, 'pair', [['x', 1]], false);
page = await catalog.browse(conn, { table: 'pair', offset: 0, limit: 10, sort: [], filters: {} }, false);
assert.deepEqual(page.rows, [['x', 2, 'deux']]);
await run('COMMIT');
ok('a WITHOUT ROWID table');

// values: integers beyond a double, reals, blobs
await run("INSERT INTO typed VALUES (9223372036854775807, 2.5, 'é', x'00ff10')");
await run('COMMIT');
page = await catalog.browse(conn, { table: 'typed', offset: 0, limit: 10, sort: [], filters: {} }, false);
assert.deepEqual(page.rows[0].slice(0, 3), [{ type: 'integer', value: '9223372036854775807' }, 2.5, 'é']);
assert.deepEqual(page.rows[0][3], { type: 'blob', size: 3, value: '00ff10' });
await catalog.updateRow(conn, 'typed', page.keys[0], { i: { type: 'integer', value: '-9223372036854775808' } }, false);
assert.equal((await rows('SELECT i FROM typed'))[0][0].value, '-9223372036854775808');
await assert.rejects(catalog.updateRow(conn, 'typed', page.keys[0], { b: page.rows[0][3] }, false), /cannot be sent back/);
await run('ROLLBACK');
ok('integers beyond a double, blobs');

// ---- pragmas
let pragmas = await catalog.loadPragmas(conn, false);
const byName = Object.fromEntries(pragmas.map(p => [p.name, p]));
assert.equal(byName.foreign_keys.kind, 'boolean');
assert.equal(byName.page_size.readOnly, true);
await catalog.setPragma(conn, 'user_version', 7, false, false);
pragmas = await catalog.loadPragmas(conn, false);
assert.equal(pragmas.find(p => p.name === 'user_version').value, 7);
await catalog.setPragma(conn, 'foreign_keys', 1, false, false);
assert.equal((await catalog.loadPragmas(conn, false)).find(p => p.name === 'foreign_keys').value, 1);
ok('pragmas');

// ---- dump
const script = await catalog.dump(conn);
assert(script.startsWith('BEGIN TRANSACTION;\n') && script.endsWith('COMMIT;\n'));
assert.match(script, /CREATE TABLE person/);
assert.match(script, /INSERT INTO "person"\("id","name","age","note"\) VALUES\(1,'Ada',36,'first'\);/);
assert.match(script, /CREATE INDEX person_name/);
// the script rebuilds the database
const copy = path.join(dir, 'copy.sqldb');
await new Promise((resolve, reject) => {
  const restore = spawn(binary, [copy, '-i'], { stdio: ['pipe', 'ignore', 'inherit'] });
  restore.on('exit', code => (code === 0 ? resolve() : reject(new Error(`restore exited with ${code}`))));
  restore.stdin.end(script);
});
const check = await new Promise(resolve => {
  let out = '';
  const reader = spawn(binary, [copy, '-c', 'SELECT count(*) FROM person', '--mode', 'list'], { stdio: ['ignore', 'pipe', 'inherit'] });
  reader.stdout.on('data', d => (out += d));
  reader.on('exit', () => resolve(out.trim()));
});
assert.match(check, /\b4\b/);
ok('the dump rebuilds the database');

// ---- the old JSON API is gone
const response = await fetch(`http://127.0.0.1:${port}/api/info`);
assert.equal(response.status, 404);
ok('no JSON API');

socket.close();
assert.equal(exited, null, 'the server is still running');
console.log('all ok');
finish(0);
