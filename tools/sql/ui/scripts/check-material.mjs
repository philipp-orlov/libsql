// Copyright (c) 2026 Philipp Orlov
// The web UI is built from Angular Material components. This fails when a template uses a native form control where a
// Material one exists: <select>, <datalist>, <textarea>/<input> without matInput, date/checkbox/radio/range inputs.
// Allowed natives: type=file (hidden, behind a Material button), type=color (Material has no color picker),
// matSliderThumb, and inputs/textareas that carry matInput.
//   node scripts/check-material.mjs
import fs from 'node:fs';
import path from 'node:path';

const root = new URL('../src/app', import.meta.url).pathname;
const files = [];
const walk = dir => {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p);
    else if (/\.(html|component\.ts)$/.test(e.name) && !/\.spec\.ts$/.test(e.name)) files.push(p);
  }
};
walk(root);

const problems = [];
for (const file of files) {
  const source = fs.readFileSync(file, 'utf8');
  for (const m of source.matchAll(/<(select|datalist|textarea|input)\b([^>]*)>/g)) {
    const [, tag, attrs] = m;
    const line = source.slice(0, m.index).split('\n').length;
    const type = /\btype="([^"]*)"/.exec(attrs)?.[1];
    if (/\bmatInput\b|\bmatSliderThumb\b|\bmatAutocomplete\b/.test(attrs)) continue;
    if (tag === 'input' && (type === 'file' || type === 'color')) continue;
    if (/\bdata-native-ok\b/.test(attrs)) continue;
    problems.push(`${path.relative(root, file)}:${line}: native <${tag}${type ? ` type="${type}"` : ''}> -- use the Material component`);
  }
}
if (problems.length) {
  console.error(problems.join('\n'));
  process.exit(1);
}
console.log(`${files.length} templates checked: no native form controls`);
