// Regenerates what the front end takes from librpc: the TypeScript for include/sql/wire/remote.bufs (rpcc --ts) into
// src/app/remote/wire, and a copy of librpc's TypeScript runtime into src/app/remote/librpc.
//   node scripts/vendor-librpc.mjs path/to/rpcc [--check]
// With --check nothing is written: it exits 1 if the committed files differ (the ctest `ui_wire_up_to_date`).
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ui = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const libsql = path.join(ui, '../../..');
const rpcc = process.argv[2];
const check = process.argv.includes('--check');
if (!rpcc) {
  console.error('usage: node scripts/vendor-librpc.mjs path/to/rpcc [--check]');
  process.exit(2);
}
const wire = path.join(libsql, 'include/sql/wire');
const runtime = path.join(libsql, '../librpc/ts/src');
const bufsRuntime = path.join(libsql, '../libbufs/ts/src');
const out = check ? fs.mkdtempSync(path.join(os.tmpdir(), 'ui-wire-')) : path.join(ui, 'src/app/remote');
fs.mkdirSync(path.join(out, 'wire'), { recursive: true });
fs.mkdirSync(path.join(out, 'librpc'), { recursive: true });
for (const [schema, extra] of [['remote.bufs', ['-I', wire]], ['resultset.bufs', []]]) {
  const r = spawnSync(rpcc, ['--no-cpp', '--ts', '--ts-runtime', '../librpc/index', '-o', path.join(out, 'wire'), ...extra, path.join(wire, schema)], { stdio: 'inherit' });
  if (r.status !== 0) process.exit(1);
}
for (const f of ['bufs', 'client', 'rpc.bufs', 'index']) {
  const from = f === 'bufs' ? bufsRuntime : runtime;   // the libbufs runtime is librpc/ts/src/bufs.ts's source
  const text = `// Vendored from ${f === 'bufs' ? 'libbufs/ts' : 'librpc/ts'}/src by scripts/vendor-librpc.mjs. Do not edit here.\n` + fs.readFileSync(path.join(from, f + '.ts'), 'utf8');
  fs.writeFileSync(path.join(out, 'librpc', f + '.ts'), text);
}
if (check) {
  let stale = false;
  for (const dir of ['wire', 'librpc']) {
    for (const f of fs.readdirSync(path.join(out, dir))) {
      const committed = path.join(ui, 'src/app/remote', dir, f);
      if (!fs.existsSync(committed) || fs.readFileSync(committed, 'utf8') !== fs.readFileSync(path.join(out, dir, f), 'utf8')) {
        console.error(`stale: src/app/remote/${dir}/${f}`);
        stale = true;
      }
    }
  }
  fs.rmSync(out, { recursive: true, force: true });
  if (stale) {
    console.error('run: node scripts/vendor-librpc.mjs path/to/rpcc');
    process.exit(1);
  }
}
