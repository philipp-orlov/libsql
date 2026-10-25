// Client decode against a running `sql <db> --listen PORT`: what the browser pays to get a result into the grid.
//   node scripts/bench-decode.mjs [port] [maxRows] [sql]
// Bundles src/app/bufs with esbuild, fetches the same query as JSON and as libbufs, and times JSON.parse (the grid
// then holds Cell[][]) against ResultSetView.open plus reading the first 50 rows (what the grid renders) or every cell.
import { build } from 'esbuild';
import { performance } from 'node:perf_hooks';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const port = process.argv[2] ?? '8080';
const maxRows = Number(process.argv[3] ?? 100000);
const sql = process.argv[4] ?? 'SELECT id, name, status, price, qty, created, note FROM orders';
const out = await build({ entryPoints: [path.join(root, 'src/app/bufs/index.ts')], bundle: true, write: false, format: 'esm', platform: 'node' });
const { ResultSetView } = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));

async function post(format) {
  const response = await fetch(`http://127.0.0.1:${port}/api/sql`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ sql, maxRows, format }),
  });
  return response.arrayBuffer();
}

const median = (fn, reps = 15) => {
  const times = [];
  for (let i = 0; i < reps; i++) {
    const a = performance.now();
    fn();
    times.push(performance.now() - a);
  }
  return times.sort((x, y) => x - y)[reps >> 1];
};

const json = await post('json');
const bufs = await post('bufs');
const text = new TextDecoder().decode(json);
const reply = JSON.parse(text);
const view0 = ResultSetView.open(bufs);
console.log(`${view0.rowCount} rows x ${view0.columns.length} columns; wire: json ${json.byteLength} bytes, bufs ${bufs.byteLength} bytes`);

const parse = median(() => JSON.parse(new TextDecoder().decode(json)));
const first50 = median(() => {
  const v = ResultSetView.open(bufs);
  for (let r = 0; r < 50; r++) for (let c = 0; c < v.columns.length; c++) v.cell(r, c);
});
let sink = 0;
const all = median(() => {
  const v = ResultSetView.open(bufs);
  for (let r = 0; r < v.rowCount; r++) for (let c = 0; c < v.columns.length; c++) sink += v.cell(r, c) === null ? 0 : 1;
});
console.log(`JSON.parse (rows become Cell[][])          ${parse.toFixed(1)} ms`);
console.log(`bufs open + read the first 50 rows         ${first50.toFixed(2)} ms`);
console.log(`bufs open + read every cell as Cell        ${all.toFixed(1)} ms (${sink})`);
