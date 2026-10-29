// Runs the framework-free specs (src/**/*.nspec.ts: the report engine) in Node, with jasmine-core and esbuild; no browser needed.
//   node scripts/unit.mjs [filter]
import { build } from 'esbuild';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ui = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const require = createRequire(import.meta.url);
const filter = process.argv[2];
const walk = dir => fs.readdirSync(dir, { withFileTypes: true }).flatMap(e => (e.isDirectory() ? walk(path.join(dir, e.name)) : [path.join(dir, e.name)]));
const specs = walk(path.join(ui, 'src')).filter(f => f.endsWith('.nspec.ts') && (!filter || f.includes(filter)));
if (!specs.length) {
  console.error('no specs');
  process.exit(1);
}
const out = fs.mkdtempSync(path.join(os.tmpdir(), 'ui-unit-'));
process.env.REPORT_FONTS ??= path.resolve(ui, '../fonts');
await build({ entryPoints: specs, bundle: true, platform: 'node', format: 'esm', outdir: out, outExtension: { '.js': '.mjs' }, logLevel: 'warning', absWorkingDir: ui });

const jasmineRequire = require('jasmine-core');
const jasmine = jasmineRequire.core(jasmineRequire);
const env = jasmine.getEnv({ suppressLoadErrors: true });
Object.assign(globalThis, jasmineRequire.interface(jasmine, env));
let failed = 0;
let passed = 0;
env.configure({ random: false });
env.addReporter({
  specDone(r) {
    if (r.status === 'failed') {
      failed++;
      console.log(`FAILED  ${r.fullName}`);
      for (const f of r.failedExpectations) console.log('        ' + f.message.split('\n')[0] + (f.stack ? '\n' + f.stack.split('\n').slice(1, 3).join('\n') : ''));
    } else if (r.status === 'passed') passed++;
  },
});
for (const f of fs.readdirSync(out)) await import(pathToFileURL(path.join(out, f)).href);
const done = await env.execute();
fs.rmSync(out, { recursive: true, force: true });
console.log(`${passed} specs passed, ${failed} failed (${done.overallStatus})`);
process.exit(failed || done.overallStatus !== 'passed' ? 1 : 0);
