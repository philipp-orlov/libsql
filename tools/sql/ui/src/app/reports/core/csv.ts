// Copyright (c) 2026 Philipp Orlov

// CSV as RFC 4180 describes it: fields with a comma, a quote or a line break are quoted, quotes are doubled, lines end
// with CRLF. null is an empty field. Free of Angular imports on purpose.

import { Value } from './format';

function field(v: Value | undefined): string {
  if (v === null || v === undefined) return '';
  const s = typeof v === 'boolean' ? (v ? 'true' : 'false') : String(v);
  return /[",\r\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
}

export function toCsv(columns: string[], rows: Value[][], options: { header?: boolean; bom?: boolean } = {}): string {
  const lines: string[] = [];
  if (options.header !== false) lines.push(columns.map(field).join(','));
  for (const row of rows) lines.push(row.map(field).join(','));
  return (options.bom ? '﻿' : '') + lines.join('\r\n') + '\r\n';
}
